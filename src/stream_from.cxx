/** Implementation of the pqxx::stream_from class.
 *
 * pqxx::stream_from enables optimized batch reads from a database table.
 *
 * Copyright (c) 2000-2020, Jeroen T. Vermeulen.
 *
 * See COPYING for copyright license.  If you did not receive a file called
 * COPYING with this source code, please notify the distributor of this
 * mistake, or contact the author.
 */
#include "pqxx-source.hxx"

#include "pqxx/stream_from"

#include "pqxx/internal/encodings.hxx"
#include "pqxx/internal/gates/connection-stream_from.hxx"
#include "pqxx/transaction_base.hxx"


std::string pqxx::stream_from::compose_query(
  pqxx::transaction_base const &tx, std::string_view table,
  std::string const &columns)
{
  constexpr std::string_view copy{"COPY "}, to_stdout{" TO STDOUT"};
  auto const escaped_table{tx.quote_name(table)};
  std::string command;
  command.reserve(
    std::size(copy) + std::size(escaped_table) + std::size(columns) + 2 +
    std::size(to_stdout));
  command += copy;
  command += escaped_table;

  if (not columns.empty())
  {
    command.push_back('(');
    command += columns;
    command.push_back(')');
  }

  command += to_stdout;
  return command;
}


namespace
{
pqxx::internal::glyph_scanner_func *
get_scanner(pqxx::transaction_base const &tx)
{
  auto const group{pqxx::internal::enc_group(tx.conn().encoding_id())};
  return pqxx::internal::get_glyph_scanner(group);
}
} // namespace


pqxx::stream_from::stream_from(
  transaction_base &tx, from_query_t, std::string_view query) :
        namedclass{"stream_from"},
        transactionfocus{tx},
        m_glyph_scanner{get_scanner(tx)}
{
  constexpr std::string_view copy{"COPY ("}, to_stdout{") TO STDOUT"};
  std::string command;
  command.reserve(std::size(copy) + std::size(query) + std::size(to_stdout));
  command += copy;
  command += query;
  command += to_stdout;
  tx.exec0(command);

  register_me();
}


pqxx::stream_from::stream_from(
  transaction_base &tx, from_table_t, std::string_view table) :
        namedclass{"stream_from", table},
        transactionfocus{tx},
        m_glyph_scanner{get_scanner(tx)}
{
  auto const command{compose_query(tx, table, "")};
  tx.exec0(command);
  register_me();
}


pqxx::stream_from::stream_from(
  transaction_base &tx, std::string_view table, std::string &&columns,
  from_table_t) :
        namedclass{"stream_from", table},
        transactionfocus{tx},
        m_glyph_scanner{get_scanner(tx)}
{
  auto const command{compose_query(tx, table, columns)};
  tx.exec0(command);
  register_me();
}


pqxx::stream_from::~stream_from() noexcept
{
  try
  {
    close();
  }
  catch (std::exception const &e)
  {
    reg_pending_error(e.what());
  }
}


pqxx::stream_from::raw_line pqxx::stream_from::get_raw_line()
{
  if (*this)
  {
    internal::gate::connection_stream_from gate{m_trans.conn()};
    try
    {
      raw_line line{gate.read_copy_line()};
      if (line.first.get() == nullptr)
        close();
      return line;
    }
    catch (std::exception const &)
    {
      close();
      throw;
    }
  }
  else
  {
    return raw_line{};
  }
}


void pqxx::stream_from::close()
{
  if (not m_finished)
  {
    m_finished = true;
    unregister_me();
  }
}


void pqxx::stream_from::complete()
{
  if (m_finished)
    return;
  try
  {
    // Flush any remaining lines - libpq will automatically close the stream
    // when it hits the end.
    bool done{false};
    while (not done)
    {
      auto [line, size] = get_raw_line();
      ignore_unused(size);
      done = not line.get();
    }
  }
  catch (broken_connection const &)
  {
    close();
    throw;
  }
  catch (std::exception const &e)
  {
    reg_pending_error(e.what());
  }
  close();
}


void pqxx::stream_from::parse_line()
{
  if (m_finished)
    return;
  auto const next_seq{m_glyph_scanner};

  m_fields.clear();

  auto const [line, line_size] = get_raw_line();
  if (line.get() == nullptr)
    m_finished = true;

  // Make room for unescaping the line.  It's a pessimistic size.
  // Unusually, we're storing terminating zeroes *inside* the string.
  // This is the only place where we modify m_row.  MAKE SURE THE BUFFER DOES
  // NOT GET RESIZED while we're working, because we're working with pointers
  // into its buffer.
  m_row.resize(line_size + 1);

  char const *line_begin{line.get()};
  char const *line_end{line_begin + line_size};
  char const *read{line_begin};

  // Output iterator for unescaped text.
  char *write{m_row.data()};

  // Beginning of current field in m_row, or nullptr for null fields.
  char const *field_begin{write};

  while (read < line_end)
  {
    auto const offset{static_cast<std::size_t>(read - line_begin)};
    auto const glyph_end{line_begin + next_seq(line_begin, line_size, offset)};
    if (glyph_end == read + 1)
    {
      // Single-byte character.
      char c{*read++};
      switch (c)
      {
      case '\t': // Field separator.
        // End the field.
        if (field_begin == nullptr)
        {
          m_fields.emplace_back();
        }
        else
        {
          // Would love to emplace_back() here, but gcc 9.1 warns about the
          // constructor not throwing.  It suggests adding "noexcept."  Which
          // we can hardly do, without std::string_view guaranteeing it.
          m_fields.push_back(zview{field_begin, write - field_begin});
          *write++ = '\0';
        }
        field_begin = write;
        break;

      case '\\': {
        // Escape sequence.
        if (read >= line_end)
          throw failure{"Row ends in backslash"};

        c = *read++;
        switch (c)
        {
        case 'N':
          // Null value.
          if (write != field_begin)
            throw failure{"Null sequence found in nonempty field"};
          field_begin = nullptr;
          // (If there's any characters _after_ the null we'll just crash.)
          break;

        case 'b': // Backspace.
          *write++ = '\b';
          break;
        case 'f': // Form feed
          *write++ = '\f';
          break;
        case 'n': // Line feed.
          *write++ = '\n';
          break;
        case 'r': // Carriage return.
          *write++ = '\r';
          break;
        case 't': // Horizontal tab.
          *write++ = '\t';
          break;
        case 'v': // Vertical tab.
          *write++ = '\v';
          break;

        default:
          // Regular character ("self-escaped").
          *write++ = c;
          break;
        }
      }
      break;

      default: *write++ = c; break;
      }
    }
    else
    {
      // Multi-byte sequence.  Never treated specially, so just append.
      while (read < glyph_end) *write++ = *read++;
    }
  }

  // End the last field here.
  if (field_begin == nullptr)
  {
    m_fields.emplace_back();
  }
  else
  {
    m_fields.push_back(zview{field_begin, write - field_begin});
    *write++ = '\0';
  }

  // DO NOT shrink m_row to fit.  We're carrying string_views pointing into
  // the buffer.  (Also, how useful would shrinking really be?)
}


std::vector<pqxx::zview> const *pqxx::stream_from::read_row()
{
  parse_line();
  return m_finished ? nullptr : &m_fields;
}
