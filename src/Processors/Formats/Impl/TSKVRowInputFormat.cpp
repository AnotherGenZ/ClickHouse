#include <Columns/IColumn.h>
#include <IO/ReadHelpers.h>
#include <Processors/Formats/Impl/TSKVRowInputFormat.h>
#include <Formats/FormatFactory.h>
#include <Formats/EscapingRuleUtils.h>
#include <DataTypes/Serializations/SerializationNullable.h>
#include <DataTypes/DataTypeString.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int INCORRECT_DATA;
    extern const int CANNOT_PARSE_ESCAPE_SEQUENCE;
    extern const int CANNOT_READ_ALL_DATA;
    extern const int CANNOT_PARSE_INPUT_ASSERTION_FAILED;
    extern const int TYPE_MISMATCH;
}


TSKVRowInputFormat::TSKVRowInputFormat(ReadBuffer & in_, SharedHeader header_, Params params_, const FormatSettings & format_settings_)
    : IRowInputFormat(std::move(header_), in_, std::move(params_)), format_settings(format_settings_), name_map(getPort().getHeader().columns())
{
    const auto & sample_block = getPort().getHeader();
    size_t num_columns = sample_block.columns();
    for (size_t i = 0; i < num_columns; ++i)
        name_map[sample_block.getByPosition(i).name] = i;        /// NOTE You could place names more cache-locally.
}


void TSKVRowInputFormat::readPrefix()
{
    /// In this format, we assume that column name cannot contain BOM,
    ///  so BOM at beginning of stream cannot be confused with name of field, and it is safe to skip it.
    skipBOMIfExists(*in);
}


/** Read the field name in the `tskv` format.
  * Return true if the field is followed by an equal sign,
  *  otherwise (field with no value) return false.
  * The reference to the field name will be written to `ref`.
  * A temporary `tmp` buffer can also be used to copy the field name to it.
  * When reading, skips the name and the equal sign after it.
  */
static bool readName(ReadBuffer & buf, StringRef & ref, String & tmp)
{
    tmp.clear();

    while (!buf.eof())
    {
        const char * next_pos = find_first_symbols<'\t', '\n', '\\', '='>(buf.position(), buf.buffer().end());

        if (next_pos == buf.buffer().end())
        {
            tmp.append(buf.position(), next_pos - buf.position());
            buf.position() = buf.buffer().end();
            buf.next();
            continue;
        }

        /// Came to the end of the name.
        if (*next_pos != '\\')
        {
            bool have_value = *next_pos == '=';
            if (tmp.empty())
            {
                /// No need to copy data, you can refer directly to the `buf`.
                ref = StringRef(buf.position(), next_pos - buf.position());
                buf.position() += next_pos + have_value - buf.position();
            }
            else
            {
                /// Copy the data to a temporary string and return a reference to it.
                tmp.append(buf.position(), next_pos - buf.position());
                buf.position() += next_pos + have_value - buf.position();
                ref = StringRef(tmp);
            }
            return have_value;
        }
        /// The name has an escape sequence.

        tmp.append(buf.position(), next_pos - buf.position());
        buf.position() += next_pos + 1 - buf.position();
        if (buf.eof())
            throw Exception(ErrorCodes::CANNOT_PARSE_ESCAPE_SEQUENCE, "Cannot parse escape sequence");

        tmp.push_back(parseEscapeSequence(*buf.position()));
        ++buf.position();
    }

    throw Exception(ErrorCodes::CANNOT_READ_ALL_DATA, "Unexpected end of stream while reading key name from TSKV format");
}


bool TSKVRowInputFormat::readRow(MutableColumns & columns, RowReadExtension & ext)
{
    if (in->eof())
        return false;

    const auto & header = getPort().getHeader();
    size_t num_columns = columns.size();

    /// Set of columns for which the values were read. The rest will be filled with default values.
    read_columns.assign(num_columns, false);
    seen_columns.assign(num_columns, false);

    if (unlikely(*in->position() == '\n'))
    {
        /// An empty string. It is permissible, but it is unclear why.
        ++in->position();
    }
    else
    {
        while (true)
        {
            StringRef name_ref;
            bool has_value = readName(*in, name_ref, name_buf);
            ssize_t index = -1;

            if (has_value)
            {
                /// NOTE Optimization is possible by caching the order of fields (which is almost always the same)
                /// and quickly checking for the next expected field, instead of searching the hash table.

                auto * it = name_map.find(name_ref);
                if (!it)
                {
                    if (!format_settings.skip_unknown_fields)
                        throw Exception(ErrorCodes::INCORRECT_DATA, "Unknown field found while parsing TSKV format: {}", name_ref.toString());

                    /// If the key is not found, skip the value.
                    NullOutput sink;
                    readEscapedStringInto<NullOutput,false>(sink, *in);
                }
                else
                {
                    index = it->getMapped();

                    if (seen_columns[index])
                        throw Exception(ErrorCodes::INCORRECT_DATA, "Duplicate field found while parsing TSKV format: {}", name_ref.toString());

                    seen_columns[index] = read_columns[index] = true;
                    const auto & type = getPort().getHeader().getByPosition(index).type;
                    const auto & serialization = serializations[index];
                    if (format_settings.null_as_default && !isNullableOrLowCardinalityNullable(type))
                        read_columns[index] = SerializationNullable::deserializeNullAsDefaultOrNestedTextEscaped(*columns[index], *in, format_settings, serialization);
                    else
                        serialization->deserializeTextEscaped(*columns[index], *in, format_settings);
                }
            }
            else
            {
                /// The only thing that can go without value is `tskv` fragment that is ignored.
                if (!(name_ref.size == 4 && 0 == memcmp(name_ref.data, "tskv", 4)))
                    throw Exception(ErrorCodes::INCORRECT_DATA, "Found field without value while parsing TSKV format: {}", name_ref.toString());
            }

            if (in->eof())
            {
                throw Exception(ErrorCodes::CANNOT_READ_ALL_DATA, "Unexpected end of stream after field in TSKV format: {}", name_ref.toString());
            }
            if (*in->position() == '\t')
            {
                ++in->position();
                continue;
            }
            if (*in->position() == '\n')
            {
                ++in->position();
                break;
            }

            /// Possibly a garbage was written into column, remove it
            if (index >= 0)
            {
                columns[index]->popBack(1);
                seen_columns[index] = read_columns[index] = false;
            }

            throw Exception(
                ErrorCodes::CANNOT_PARSE_INPUT_ASSERTION_FAILED, "Found garbage after field in TSKV format: {}", name_ref.toString());
        }
    }

    /// Fill in the not met columns with default values.
    for (size_t i = 0; i < num_columns; ++i)
        if (!seen_columns[i])
        {
            const auto & type = header.getByPosition(i).type;
            if (format_settings.force_null_for_omitted_fields && !isNullableOrLowCardinalityNullable(type))
                throw Exception(
                    ErrorCodes::TYPE_MISMATCH,
                    "Cannot insert NULL value into a column `{}` of type '{}'",
                    header.getByPosition(i).name,
                    type->getName());
            type->insertDefaultInto(*columns[i]);
        }

    /// return info about defaults set
    if (format_settings.defaults_for_omitted_fields)
        ext.read_columns = read_columns;
    else
        ext.read_columns.assign(num_columns, true);

    return true;
}


void TSKVRowInputFormat::syncAfterError()
{
    skipToUnescapedNextLineOrEOF(*in);
}


void TSKVRowInputFormat::resetParser()
{
    IRowInputFormat::resetParser();
    read_columns.clear();
    seen_columns.clear();
    name_buf.clear();
}

size_t TSKVRowInputFormat::countRows(size_t max_block_size)
{
    size_t num_rows = 0;
    while (!in->eof() && num_rows < max_block_size)
    {
        skipToUnescapedNextLineOrEOF(*in);
        ++num_rows;
    }

    return num_rows;
}

TSKVSchemaReader::TSKVSchemaReader(ReadBuffer & in_, const FormatSettings & format_settings_)
    : IRowWithNamesSchemaReader(in_, format_settings_, getDefaultDataTypeForEscapingRule(FormatSettings::EscapingRule::Escaped))
{
}

NamesAndTypesList TSKVSchemaReader::readRowAndGetNamesAndDataTypes(bool & eof)
{
    if (first_row)
    {
        skipBOMIfExists(in);
        first_row = false;
    }

    if (in.eof())
    {
        eof = true;
        return {};
    }

    if (*in.position() == '\n')
    {
        ++in.position();
        return {};
    }

    NamesAndTypesList names_and_types;
    StringRef name_ref;
    String name_buf;
    String value;
    do
    {
        bool has_value = readName(in, name_ref, name_buf);
        String name = String(name_ref);
        if (has_value)
        {
            readEscapedString(value, in);
            names_and_types.emplace_back(std::move(name), tryInferDataTypeByEscapingRule(value, format_settings, FormatSettings::EscapingRule::Escaped));
        }
        else
        {
            /// The only thing that can go without value is `tskv` fragment that is ignored.
            if (!(name_ref.size == 4 && 0 == memcmp(name_ref.data, "tskv", 4)))
                throw Exception(ErrorCodes::INCORRECT_DATA, "Found field without value while parsing TSKV format: {}", name_ref.toString());
        }

    }
    while (checkChar('\t', in));

    assertChar('\n', in);

    return names_and_types;
}

void registerInputFormatTSKV(FormatFactory & factory)
{
    factory.registerInputFormat("TSKV", [](
        ReadBuffer & buf,
        const Block & sample,
        IRowInputFormat::Params params,
        const FormatSettings & settings)
    {
        return std::make_shared<TSKVRowInputFormat>(buf, std::make_shared<const Block>(sample), std::move(params), settings);
    });

    factory.markFormatSupportsSubsetOfColumns("TSKV");
}
void registerTSKVSchemaReader(FormatFactory & factory)
{
    factory.registerSchemaReader("TSKV", [](ReadBuffer & buf, const FormatSettings & settings)
    {
        return std::make_shared<TSKVSchemaReader>(buf, settings);
    });
    factory.registerAdditionalInfoForSchemaCacheGetter("TSKV", [](const FormatSettings & settings)
    {
        return getAdditionalFormatInfoByEscapingRule(settings, FormatSettings::EscapingRule::Escaped);
    });
}

}
