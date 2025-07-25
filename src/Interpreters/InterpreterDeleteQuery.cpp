#include <Interpreters/InterpreterDeleteQuery.h>
#include <Interpreters/InterpreterFactory.h>

#include <Access/ContextAccess.h>
#include <Core/Settings.h>
#include <Core/ServerSettings.h>
#include <Databases/DatabaseReplicated.h>
#include <Databases/IDatabase.h>
#include <Interpreters/Context.h>
#include <Interpreters/DatabaseCatalog.h>
#include <Interpreters/FunctionNameNormalizer.h>
#include <Interpreters/InterpreterAlterQuery.h>
#include <Interpreters/InterpreterUpdateQuery.h>
#include <Interpreters/MutationsInterpreter.h>
#include <Parsers/parseQuery.h>
#include <Parsers/ParserAlterQuery.h>
#include <Parsers/ParserUpdateQuery.h>
#include <Parsers/ASTDeleteQuery.h>
#include <Parsers/ASTUpdateQuery.h>
#include <Storages/AlterCommands.h>
#include <Storages/IStorage.h>
#include <Storages/MutationCommands.h>
#include <Storages/MergeTree/MergeTreeSettings.h>


namespace DB
{
namespace Setting
{
    extern const SettingsBool enable_lightweight_delete;
    extern const SettingsUInt64 lightweight_deletes_sync;
    extern const SettingsSeconds lock_acquire_timeout;
    extern const SettingsLightweightDeleteMode lightweight_delete_mode;
    extern const SettingsBool allow_experimental_lightweight_update;
}

namespace MergeTreeSetting
{
    extern const MergeTreeSettingsLightweightMutationProjectionMode lightweight_mutation_projection_mode;
}

namespace ServerSetting
{
    extern const ServerSettingsBool disable_insertion_and_mutation;
}

namespace ErrorCodes
{
    extern const int TABLE_IS_READ_ONLY;
    extern const int SUPPORT_IS_DISABLED;
    extern const int BAD_ARGUMENTS;
    extern const int QUERY_IS_PROHIBITED;
}


InterpreterDeleteQuery::InterpreterDeleteQuery(const ASTPtr & query_ptr_, ContextPtr context_) : WithContext(context_), query_ptr(query_ptr_)
{
}

BlockIO InterpreterDeleteQuery::execute()
{
    FunctionNameNormalizer::visit(query_ptr.get());
    const ASTDeleteQuery & delete_query = query_ptr->as<ASTDeleteQuery &>();
    auto table_id = getContext()->resolveStorageID(delete_query, Context::ResolveOrdinary);

    getContext()->checkAccess(AccessType::ALTER_DELETE, table_id);
    const auto & settings = getContext()->getSettingsRef();

    query_ptr->as<ASTDeleteQuery &>().setDatabase(table_id.database_name);

    /// First check table storage for validations.
    StoragePtr table = DatabaseCatalog::instance().getTable(table_id, getContext());
    checkStorageSupportsTransactionsIfNeeded(table, getContext());
    if (table->isStaticStorage())
        throw Exception(ErrorCodes::TABLE_IS_READ_ONLY, "Table is read-only");

    if (getContext()->getGlobalContext()->getServerSettings()[ServerSetting::disable_insertion_and_mutation])
        throw Exception(ErrorCodes::QUERY_IS_PROHIBITED, "Delete queries are prohibited");

    DatabasePtr database = DatabaseCatalog::instance().getDatabase(table_id.database_name);
    if (database->shouldReplicateQuery(getContext(), query_ptr))
    {
        auto guard = DatabaseCatalog::instance().getDDLGuard(table_id.database_name, table_id.table_name);
        guard->releaseTableLock();
        return database->tryEnqueueReplicatedDDL(query_ptr, getContext(), {});
    }

    auto table_lock = table->lockForShare(getContext()->getCurrentQueryId(), settings[Setting::lock_acquire_timeout]);
    auto metadata_snapshot = table->getInMemoryMetadataPtr();

    if (table->supportsDelete())
    {
        /// Convert to MutationCommand
        MutationCommands mutation_commands;
        MutationCommand mut_command;

        mut_command.type = MutationCommand::Type::DELETE;
        mut_command.predicate = delete_query.predicate;

        mutation_commands.emplace_back(mut_command);

        table->checkMutationIsPossible(mutation_commands, getContext()->getSettingsRef());
        MutationsInterpreter::Settings mutation_settings(false);
        MutationsInterpreter(table, metadata_snapshot, mutation_commands, getContext(), mutation_settings).validate();
        table->mutate(mutation_commands, getContext());
        return {};
    }

    if (table->supportsLightweightDelete())
    {
        if (!settings[Setting::enable_lightweight_delete])
            throw Exception(ErrorCodes::SUPPORT_IS_DISABLED,
                "Lightweight delete mutate is disabled. Set `enable_lightweight_delete` setting to enable it");

        if (metadata_snapshot->hasProjections())
        {
            if (const auto * merge_tree_data = dynamic_cast<const MergeTreeData *>(table.get()))
                if ((*merge_tree_data->getSettings())[MergeTreeSetting::lightweight_mutation_projection_mode] == LightweightMutationProjectionMode::THROW)
                    throw Exception(ErrorCodes::SUPPORT_IS_DISABLED,
                        "DELETE query is not allowed for table {} because as it has projections and setting "
                        "lightweight_mutation_projection_mode is set to THROW. "
                        "User should change lightweight_mutation_projection_mode OR "
                        "drop all the projections manually before running the query",
                        table_id.getFullTableName());
        }

        using enum LightweightDeleteMode;
        auto lightweight_delete_mode = settings[Setting::lightweight_delete_mode];

        auto supports_lightweight_update = [&] -> std::expected<void, PreformattedMessage>
        {
            if (!settings[Setting::allow_experimental_lightweight_update])
                return std::unexpected(PreformattedMessage::create("Lightweight updates are not allowed. Set 'allow_experimental_lightweight_update = 1' to allow them"));

            return table->supportsLightweightUpdate();
        }();

        if (!supports_lightweight_update && lightweight_delete_mode == LIGHTWEIGHT_UPDATE_FORCE)
        {
            throw Exception(ErrorCodes::SUPPORT_IS_DISABLED,
                "Setting lightweight_delete_mode='{}' but cannot execute query '{}' using a lightweight update. {}",
                lightweight_delete_mode.toString(), query_ptr->formatForErrorMessage(), supports_lightweight_update.error().text);
        }
        else if (supports_lightweight_update && lightweight_delete_mode != ALTER_UPDATE)
        {
            /// Build "UPDATE <table> [ON CLUSTER <cluster>] SET _row_exists = 0 [IN PARTITION <partition_id>] WHERE <predicate>" query
            static constexpr auto update_query_template = "UPDATE {}{} SET `_row_exists` = 0{} WHERE {}";

            String update_query = fmt::format(
                update_query_template,
                table->getStorageID().getFullTableName(),
                delete_query.cluster.empty() ? "" : " ON CLUSTER " + backQuoteIfNeed(delete_query.cluster),
                delete_query.partition ? " IN PARTITION " + delete_query.partition->formatWithSecretsOneLine() : "",
                delete_query.predicate->formatWithSecretsOneLine());

            ParserUpdateQuery parser;
            ASTPtr update_ast = parseQuery(
                parser,
                update_query.data(),
                update_query.data() + update_query.size(),
                "UPDATE query",
                0,
                DBMS_DEFAULT_MAX_PARSER_DEPTH,
                DBMS_DEFAULT_MAX_PARSER_BACKTRACKS);

            InterpreterUpdateQuery update_interpreter(update_ast, getContext());
            return update_interpreter.execute();
        }
        else
        {
            /// Build "ALTER <table> [ON CLUSTER <cluster>] UPDATE _row_exists = 0 [IN PARTITION <partition_id>] WHERE <predicate>" query
            static constexpr auto alter_query_template = "ALTER TABLE {}{} UPDATE `_row_exists` = 0{} WHERE {}";

            String alter_query = fmt::format(
                alter_query_template,
                table->getStorageID().getFullTableName(),
                delete_query.cluster.empty() ? "" : " ON CLUSTER " + backQuoteIfNeed(delete_query.cluster),
                delete_query.partition ? " IN PARTITION " + delete_query.partition->formatWithSecretsOneLine() : "",
                delete_query.predicate->formatWithSecretsOneLine());

            ParserAlterQuery parser;
            ASTPtr alter_ast = parseQuery(
                parser,
                alter_query.data(),
                alter_query.data() + alter_query.size(),
                "ALTER query",
                0,
                DBMS_DEFAULT_MAX_PARSER_DEPTH,
                DBMS_DEFAULT_MAX_PARSER_BACKTRACKS);

            auto context = Context::createCopy(getContext());
            context->setSetting("mutations_sync", Field(context->getSettingsRef()[Setting::lightweight_deletes_sync]));
            InterpreterAlterQuery alter_interpreter(alter_ast, context);
            return alter_interpreter.execute();
        }
    }

    throw Exception(ErrorCodes::BAD_ARGUMENTS, "DELETE query is not supported for table {}", table->getStorageID().getFullTableName());
}

void registerInterpreterDeleteQuery(InterpreterFactory & factory)
{
    auto create_fn = [](const InterpreterFactory::Arguments & args)
    {
        return std::make_unique<InterpreterDeleteQuery>(args.query, args.context);
    };

    factory.registerInterpreter("InterpreterDeleteQuery", create_fn);
}

}
