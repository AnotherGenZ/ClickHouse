#include <Parsers/ASTDropQuery.h>

#include <Parsers/CommonParsers.h>
#include <Parsers/ParserDropQuery.h>
#include <Parsers/ParserCreateQuery.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int SYNTAX_ERROR;
}

namespace
{

bool parseDropQuery(IParser::Pos & pos, ASTPtr & node, Expected & expected, const ASTDropQuery::Kind kind)
{
    ParserKeyword s_temporary(Keyword::TEMPORARY);
    ParserKeyword s_table(Keyword::TABLE);
    ParserKeyword s_dictionary(Keyword::DICTIONARY);
    ParserKeyword s_view(Keyword::VIEW);
    ParserKeyword s_database(Keyword::DATABASE);
    ParserKeyword s_from(Keyword::FROM);
    ParserKeyword s_all(Keyword::ALL);
    ParserKeyword s_tables(Keyword::TABLES);
    ParserKeyword s_not(Keyword::NOT);
    ParserKeyword s_like(Keyword::LIKE);
    ParserKeyword s_ilike(Keyword::ILIKE);
    ParserToken s_dot(TokenType::Dot);
    ParserKeyword s_if_exists(Keyword::IF_EXISTS);
    ParserKeyword s_if_empty(Keyword::IF_EMPTY);
    ParserIdentifier name_p(true);
    ParserStringLiteral like_p(Highlight::string_like);
    ParserKeyword s_permanently(Keyword::PERMANENTLY);
    ParserKeyword s_no_delay(Keyword::NO_DELAY);
    ParserKeyword s_sync(Keyword::SYNC);
    ParserNameList tables_p;

    ASTPtr database;
    ASTPtr database_and_tables;
    String cluster_str;
    ASTPtr like;
    bool if_exists = false;
    bool if_empty = false;
    bool has_tables = false;
    bool is_like = false;
    bool is_not_like = false;
    bool is_case_insensitive_like = false;
    bool has_all = false;
    bool temporary = false;
    bool is_dictionary = false;
    bool is_view = false;
    bool sync = false;
    bool permanently = false;

    if (s_all.checkWithoutMoving(pos, expected))
        has_all = true;

    if (s_database.ignore(pos, expected))
    {
        if (s_if_exists.ignore(pos, expected))
            if_exists = true;

        if (s_if_empty.ignore(pos, expected))
            if_empty = true;

        if (!name_p.parse(pos, database, expected))
            return false;
    }
    else if ((s_tables.ignore(pos, expected) || (s_all.ignore(pos, expected) && s_tables.ignore(pos, expected))) && kind == ASTDropQuery::Kind::Truncate)
    {
        /// Either 'TRUNCATE TABLES FROM ..' or 'TRUNCATE ALL TABLES FROM ..'
        has_tables = true;
        if (!s_from.ignore(pos, expected))
            return false;

        if (s_if_exists.ignore(pos, expected))
            if_exists = true;

        if (!name_p.parse(pos, database, expected))
            return false;

        bool not_like = false;
        if (s_not.ignore(pos, expected))
            not_like = true;

        if (s_like.ignore(pos, expected))
        {
            if (not_like)
                is_not_like = true;
            if (!like_p.parse(pos, like, expected))
                return false;
            is_like = true;
        }

        if (s_ilike.ignore(pos, expected))
        {
            is_case_insensitive_like = true;
            if (not_like)
                is_not_like = true;
            if (!like_p.parse(pos, like, expected))
                return false;
            is_like = true;
        }
    }
    else
    {
        if (s_view.ignore(pos, expected))
            is_view = true;
        else if (s_dictionary.ignore(pos, expected))
            is_dictionary = true;
        else if (s_temporary.ignore(pos, expected))
            temporary = true;

        /// for TRUNCATE queries TABLE keyword is assumed as default and can be skipped
        if (!is_view && !is_dictionary && (!s_table.ignore(pos, expected) && kind != ASTDropQuery::Kind::Truncate))
        {
            return false;
        }

        if (s_if_exists.ignore(pos, expected))
            if_exists = true;

        if (s_if_empty.ignore(pos, expected))
            if_empty = true;

        if (!tables_p.parse(pos, database_and_tables, expected))
            return false;

        if (database_and_tables->as<ASTExpressionList &>().children.size() > 1 && kind != ASTDropQuery::Kind::Drop)
            throw Exception(ErrorCodes::SYNTAX_ERROR, "Only Support DROP multiple tables currently");
    }

    /// common for tables / dictionaries / databases
    if (ParserKeyword{Keyword::ON}.ignore(pos, expected))
    {
        if (!ASTQueryWithOnCluster::parse(pos, cluster_str, expected))
            return false;
    }

    if (kind == ASTDropQuery::Kind::Detach && s_permanently.ignore(pos, expected))
        permanently = true;

    /// actually for TRUNCATE NO DELAY / SYNC means nothing
    if (s_no_delay.ignore(pos, expected) || s_sync.ignore(pos, expected))
        sync = true;

    auto query = std::make_shared<ASTDropQuery>();
    node = query;

    query->kind = kind;
    query->if_exists = if_exists;
    query->if_empty = if_empty;
    query->has_tables = has_tables;
    query->has_all = has_all;
    query->temporary = temporary;
    query->is_dictionary = is_dictionary;
    query->is_view = is_view;
    query->sync = sync;
    query->permanently = permanently;
    query->database = database;
    query->database_and_tables = database_and_tables;
    query->case_insensitive_like = is_case_insensitive_like;
    query->not_like = is_not_like;

    if (database)
        query->children.push_back(database);

    if (database_and_tables)
        query->children.push_back(database_and_tables);

    if (is_like)
        query->like = like->as<ASTLiteral &>().value.safeGet<String>();

    query->cluster = cluster_str;

    if (database_and_tables && database_and_tables->as<ASTExpressionList &>().children.size() == 1)
        node = query->getRewrittenASTsOfSingleTable()[0];

    return true;
}

}

bool ParserDropQuery::parseImpl(Pos & pos, ASTPtr & node, Expected & expected)
{
    ParserKeyword s_drop(Keyword::DROP);
    ParserKeyword s_detach(Keyword::DETACH);
    ParserKeyword s_truncate(Keyword::TRUNCATE);

    if (s_drop.ignore(pos, expected))
        return parseDropQuery(pos, node, expected, ASTDropQuery::Kind::Drop);
    if (s_detach.ignore(pos, expected))
        return parseDropQuery(pos, node, expected, ASTDropQuery::Kind::Detach);
    if (s_truncate.ignore(pos, expected))
        return parseDropQuery(pos, node, expected, ASTDropQuery::Kind::Truncate);
    return false;
}

}
