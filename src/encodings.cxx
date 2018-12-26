/** Implementation of string encodings support
 *
 * Copyright (c) 2001-2018, Jeroen T. Vermeulen.
 *
 * See COPYING for copyright license.  If you did not receive a file called
 * COPYING with this source code, please notify the distributor of this mistake,
 * or contact the author.
 */
#include "pqxx/compiler-internal.hxx"

#include "pqxx/except.hxx"
#include "pqxx/internal/encodings.hxx"

#include <iomanip>
#include <map>
#include <sstream>

using namespace pqxx::internal;

extern "C"
{
#include "libpq-fe.h"
// These headers would be needed (in this order) to use the libpq encodings enum
// directly, which the pg_wchar.h header explicitly warns against doing:
// #include "internal/c.h"
// #include "server/mb/pg_wchar.h"
}


// Internal helper functions
namespace
{
/// Extract byte from buffer, return as unsigned char.
unsigned char get_byte(const char buffer[], std::string::size_type offset)
{
  return static_cast<unsigned char>(buffer[offset]);
}


[[noreturn]] void throw_for_encoding_error(
  const char* encoding_name,
  const char buffer[],
  std::string::size_type start,
  std::string::size_type count
)
{
  std::stringstream s;
  s
    << "Invalid byte sequence for encoding "
    << encoding_name
    << " at byte "
    << start
    << ": "
    << std::hex
    << std::setw(2)
    << std::setfill('0')
  ;
  for (std::string::size_type i{0}; i < count; ++i)
  {
    s << "0x" << static_cast<unsigned int>(get_byte(buffer, start + i));
    if (i + 1 < count) s << " ";
  }
  throw pqxx::argument_error{s.str()};
}


/// Does value lie between bottom and top, inclusive?
constexpr bool between_inc(unsigned char value, unsigned bottom, unsigned top)
{
  return value >= bottom and value <= top;
}


/*
EUC-JP and EUC-JIS-2004 represent slightly different code points but iterate
the same:
 * https://en.wikipedia.org/wiki/Extended_Unix_Code#EUC-JP
 * http://x0213.org/codetable/index.en.html
*/
std::string::size_type next_seq_for_euc_jplike(
	const char buffer[],
	std::string::size_type buffer_len,
	std::string::size_type start,
	const char encoding_name[])
{
  if (start >= buffer_len)
    return std::string::npos;

  const auto first_byte = get_byte(buffer, start);
  if (first_byte < 0x80) return start + 1;

  if (start + 2 > buffer_len)
    throw_for_encoding_error(encoding_name, buffer, start, 1);

  const auto second_byte = get_byte(buffer, start + 1);
  if (first_byte == 0x8e)
  {
    if (not between_inc(second_byte, 0xa1, 0xfe))
      throw_for_encoding_error(encoding_name, buffer, start, 2);

    return start + 2;
  }

  if (between_inc(first_byte, 0xa1, 0xfe))
  {
    if (not between_inc(second_byte, 0xa1, 0xfe))
      throw_for_encoding_error(encoding_name, buffer, start, 2);

    return start + 2;
  }

  if (first_byte == 0x8f and start + 3 <= buffer_len)
  {
    const auto third_byte = get_byte(buffer, start + 2);
    if (
	not between_inc(second_byte, 0xa1, 0xfe) or
        not between_inc(third_byte, 0xa1, 0xfe)
      )
      throw_for_encoding_error(encoding_name, buffer, start, 3);

    return start + 3;
  }

  throw_for_encoding_error(encoding_name, buffer, start, 1);
}

/*
As far as I can tell, for the purposes of iterating the only difference between
SJIS and SJIS-2004 is increased range in the first byte of two-byte sequences
(0xEF increased to 0xFC).  Officially, that is; apparently the version of SJIS
used by Postgres has the same range as SJIS-2004.  They both have increased
range over the documented versions, not having the even/odd restriction for the
first byte in 2-byte sequences.
*/
// https://en.wikipedia.org/wiki/Shift_JIS#Shift_JIS_byte_map
// http://x0213.org/codetable/index.en.html
std::string::size_type next_seq_for_sjislike(
  const char buffer[],
  std::string::size_type buffer_len,
  std::string::size_type start,
  const char* encoding_name
)
{
  if (start >= buffer_len)
    return std::string::npos;

  const auto first_byte = get_byte(buffer, start);
  if (first_byte < 0x80 or between_inc(first_byte, 0xa1, 0xdf))
    return start + 1;

  if (
	not between_inc(first_byte, 0x81, 0x9f) and
	not between_inc(first_byte, 0xe0, 0xfc)
  )
    throw_for_encoding_error(encoding_name, buffer, start, 1);

  if (start + 2 > buffer_len)
    throw_for_encoding_error(
	encoding_name,
	buffer,
	start,
	buffer_len - start);

  const auto second_byte = get_byte(buffer, start + 1);
  if (second_byte == 0x7f)
    throw_for_encoding_error(encoding_name, buffer, start, 2);

  if (
	between_inc(second_byte, 0x40, 0x9e) or
	between_inc(second_byte, 0x9f, 0xfc)
  )
    return start + 2;

  throw_for_encoding_error(encoding_name, buffer, start, 2);
}
} // namespace


// Implement template specializations first
namespace pqxx
{
namespace internal
{

template<> std::string::size_type next_seq<encoding_group::MONOBYTE>(
  const char /* buffer */[],
  std::string::size_type buffer_len,
  std::string::size_type start
)
{
  if (start >= buffer_len)
    return std::string::npos;
  else
    return start + 1;
}

// https://en.wikipedia.org/wiki/Big5#Organization
template<> std::string::size_type next_seq<encoding_group::BIG5>(
  const char buffer[],
  std::string::size_type buffer_len,
  std::string::size_type start
)
{
  if (start >= buffer_len)
    return std::string::npos;

  const auto first_byte = get_byte(buffer, start);
  if (first_byte < 0x80)
    return start + 1;

  if (not between_inc(first_byte, 0x81, 0xfe) or (start + 2 > buffer_len))
    throw_for_encoding_error("BIG5", buffer, start, 1);

  const auto second_byte = get_byte(buffer, start + 1);
  if (
	not between_inc(second_byte, 0x40, 0x7e) and
	not between_inc(second_byte, 0xa1, 0xfe))
    throw_for_encoding_error("BIG5", buffer, start, 2);

  return start + 2;
}

/*
The PostgreSQL documentation claims that the EUC_* encodings are 1-3 bytes each,
but other documents explain that the EUC sets can contain 1-(2,3,4) bytes
depending on the specific extension:
    EUC_CN      : 1-2
    EUC_JP      : 1-3
    EUC_JIS_2004: 1-2
    EUC_KR      : 1-2
    EUC_TW      : 1-4
*/

// https://en.wikipedia.org/wiki/GB_2312#EUC-CN
template<> std::string::size_type next_seq<encoding_group::EUC_CN>(
  const char buffer[],
  std::string::size_type buffer_len,
  std::string::size_type start
)
{
  if (start >= buffer_len)
    return std::string::npos;

  const auto first_byte = get_byte(buffer, start);
  if (first_byte < 0x80)
    return start + 1;

  if (not between_inc(first_byte, 0xa1, 0xf7) or start + 2 > buffer_len)
    throw_for_encoding_error("EUC_CN", buffer, start, 1);

  const auto second_byte = get_byte(buffer, start + 1);
  if (not between_inc(second_byte, 0xa1, 0xfe))
    throw_for_encoding_error("EUC_CN", buffer, start, 2);

  return start + 2;
}

template<> std::string::size_type next_seq<encoding_group::EUC_JP>(
  const char buffer[],
  std::string::size_type buffer_len,
  std::string::size_type start
)
{
  return next_seq_for_euc_jplike(buffer, buffer_len, start, "EUC_JP");
}

template<> std::string::size_type next_seq<encoding_group::EUC_JIS_2004>(
  const char buffer[],
  std::string::size_type buffer_len,
  std::string::size_type start
)
{
  return next_seq_for_euc_jplike(buffer, buffer_len, start, "EUC_JIS_2004");
}

// https://en.wikipedia.org/wiki/Extended_Unix_Code#EUC-KR
template<> std::string::size_type next_seq<encoding_group::EUC_KR>(
  const char buffer[],
  std::string::size_type buffer_len,
  std::string::size_type start
)
{
  if (start >= buffer_len)
    return std::string::npos;

  const auto first_byte = get_byte(buffer, start);
  if (first_byte < 0x80)
    return start + 1;

  if (not between_inc(first_byte, 0xa1, 0xfe) or start + 2 > buffer_len)
    throw_for_encoding_error("EUC_KR", buffer, start, 1);

  const auto second_byte = get_byte(buffer, start + 1);
  if (not between_inc(second_byte, 0xa1, 0xfe))
    throw_for_encoding_error("EUC_KR", buffer, start, 1);

  return start + 2;
}

// https://en.wikipedia.org/wiki/Extended_Unix_Code#EUC-TW
template<> std::string::size_type next_seq<encoding_group::EUC_TW>(
  const char buffer[],
  std::string::size_type buffer_len,
  std::string::size_type start
)
{
  if (start >= buffer_len)
    return std::string::npos;

  const auto first_byte = get_byte(buffer, start);
  if (first_byte < 0x80)
    return start + 1;

  if (start + 2 > buffer_len)
    throw_for_encoding_error("EUC_KR", buffer, start, 1);

  const auto second_byte = get_byte(buffer, start + 1);
  if (between_inc(first_byte, 0xa1, 0xfe))
  {
    if (not between_inc(second_byte, 0xa1, 0xfe))
      throw_for_encoding_error("EUC_KR", buffer, start, 2);

    return start + 2;
  }

  if (first_byte != 0x8e or start + 4 > buffer_len)
    throw_for_encoding_error("EUC_KR", buffer, start, 1);

  if (
            between_inc(second_byte, 0xa1, 0xb0)
        and between_inc(get_byte(buffer, start + 2), 0xa1, 0xfe)
        and between_inc(get_byte(buffer, start + 3), 0xa1, 0xfe)
  )
    return start + 4;

  throw_for_encoding_error("EUC_KR", buffer, start, 4);
}

// https://en.wikipedia.org/wiki/GB_18030#Mapping
template<> std::string::size_type next_seq<encoding_group::GB18030>(
  const char buffer[],
  std::string::size_type buffer_len,
  std::string::size_type start
)
{
  if (start >= buffer_len)
    return std::string::npos;

  const auto first_byte = get_byte(buffer, start);
  if (between_inc(first_byte, 0x80, 0xff))
    return start + 1;

  if (start + 2 > buffer_len)
    throw_for_encoding_error("GB18030", buffer, start, buffer_len - start);

  const auto second_byte = get_byte(buffer, start + 1);
  if (between_inc(second_byte, 0x40, 0xfe))
  {
    if (second_byte == 0x7f)
      throw_for_encoding_error("GB18030", buffer, start, 2);

    return start + 2;
  }

  if (start + 4 > buffer_len)
    throw_for_encoding_error("GB18030", buffer, start, buffer_len - start);

  if (
	between_inc(second_byte, 0x30, 0x39) and
	between_inc(get_byte(buffer, start + 2), 0x81, 0xfe) and
	between_inc(get_byte(buffer, start + 3), 0x30, 0x39)
  )
    return start + 4;

  throw_for_encoding_error("GB18030", buffer, start, 4);
}

// https://en.wikipedia.org/wiki/GBK_(character_encoding)#Encoding
template<> std::string::size_type next_seq<encoding_group::GBK>(
  const char buffer[],
  std::string::size_type buffer_len,
  std::string::size_type start
)
{
  if (start >= buffer_len)
    return std::string::npos;

  if (static_cast<unsigned char>(buffer[start]) < 0x80)
    return start + 1;

  if (start + 2 > buffer_len)
    throw_for_encoding_error("GBK", buffer, start, 1);

  const auto first_byte = get_byte(buffer, start);
  const auto second_byte = get_byte(buffer, start + 1);
  if (
    (
	between_inc(first_byte, 0xa1, 0xa9) and
        between_inc(second_byte, 0xa1, 0xfe)
    ) or (
	between_inc(first_byte, 0xb0, 0xf7) and
        between_inc(second_byte, 0xa1, 0xfe)
      ) or (
        between_inc(first_byte, 0x81, 0xa0) and
        between_inc(second_byte, 0x40, 0xfe) and
        second_byte != 0x7f
      ) or (
        between_inc(first_byte, 0xaa, 0xfe) and
        between_inc(second_byte, 0x40, 0xa0) and
        second_byte != 0x7f
      ) or (
        between_inc(first_byte, 0xa8, 0xa9) and
        between_inc(second_byte, 0x40, 0xa0) and
        second_byte != 0x7f
      ) or (
        between_inc(first_byte, 0xaa, 0xaf) and
        between_inc(second_byte, 0xa1, 0xfe)
      ) or (
        between_inc(first_byte, 0xf8, 0xfe) and
        between_inc(second_byte, 0xa1, 0xfe)
      ) or (
        between_inc(first_byte, 0xa1, 0xa7) and
        between_inc(second_byte, 0x40, 0xa0) and
        second_byte != 0x7f
  ))
    return start + 2;

  throw_for_encoding_error("GBK", buffer, start, 2);
}

/*
The PostgreSQL documentation claims that the JOHAB encoding is 1-3 bytes, but
"CJKV Information Processing" describes it (actually just the Hangul portion)
as "three five-bit segments" that reside inside 16 bits (2 bytes).

CJKV Information Processing by Ken Lunde, pg. 269:

  https://bit.ly/2BEOu5V
*/
template<> std::string::size_type next_seq<encoding_group::JOHAB>(
  const char buffer[],
  std::string::size_type buffer_len,
  std::string::size_type start
)
{
  if (start >= buffer_len)
    return std::string::npos;

  const auto first_byte = get_byte(buffer, start);
  if (first_byte < 0x80)
    return start + 1;

  if (start + 2 > buffer_len)
    throw_for_encoding_error("JOHAB", buffer, start, 1);

  const auto second_byte = get_byte(buffer, start);
  if (
    (
      between_inc(first_byte, 0x84, 0xd3) and
      (
        between_inc(second_byte, 0x41, 0x7e) or
        between_inc(second_byte, 0x81, 0xfe)
      )
    )
    or
    (
      (
        between_inc(first_byte, 0xd8, 0xde) or
        between_inc(first_byte, 0xe0, 0xf9)
      )
      and
      (
        between_inc(second_byte, 0x31, 0x7e) or
        between_inc(second_byte, 0x91, 0xfe)
      )
    )
  )
    return start + 2;

  throw_for_encoding_error("JOHAB", buffer, start, 2);
}

/*
PostgreSQL's MULE_INTERNAL is the emacs rather than Xemacs implementation;
see the server/mb/pg_wchar.h PostgreSQL header file.
This is implemented according to the description in said header file, but I was
unable to get it to successfully iterate a MULE-encoded test CSV generated using
PostgreSQL 9.2.23.  Use this at your own risk.
*/
template<> std::string::size_type next_seq<encoding_group::MULE_INTERNAL>(
  const char buffer[],
  std::string::size_type buffer_len,
  std::string::size_type start
)
{
  if (start >= buffer_len)
    return std::string::npos;

  const auto first_byte = get_byte(buffer, start);
  if (first_byte < 0x80)
    return start + 1;

  if (start + 2 > buffer_len)
    throw_for_encoding_error("MULE_INTERNAL", buffer, start, 1);

  const auto second_byte = get_byte(buffer, start + 1);
  if (between_inc(first_byte, 0x81, 0x8d) and second_byte >= 0xA0)
    return start + 2;

  if (start + 3 > buffer_len)
    throw_for_encoding_error("MULE_INTERNAL", buffer, start, 2);

  if (
    (
      (first_byte == 0x9A and between_inc(second_byte, 0xa0, 0xdf)) or
      (first_byte == 0x9B and between_inc(second_byte, 0xe0, 0xef)) or
      (between_inc(first_byte, 0x90, 0x99) and second_byte >= 0xa0)
    )
    and
    (
      second_byte >= 0xA0
    )
  )
    return start + 3;

  if (start + 4 > buffer_len)
    throw_for_encoding_error("MULE_INTERNAL", buffer, start, 3);

  if (
    (
      (first_byte == 0x9C and between_inc(second_byte, 0xf0, 0xf4)) or
      (first_byte == 0x9D and between_inc(second_byte, 0xf5, 0xfe))
    )
    and
    get_byte(buffer, start + 2) >= 0xa0 and
    get_byte(buffer, start + 4) >= 0xa0
  )
    return start + 4;

  throw_for_encoding_error("MULE_INTERNAL", buffer, start, 4);
}

template<> std::string::size_type next_seq<encoding_group::SJIS>(
  const char buffer[],
  std::string::size_type buffer_len,
  std::string::size_type start
)
{
  return next_seq_for_sjislike(buffer, buffer_len, start, "SJIS");
}

template<> std::string::size_type next_seq<encoding_group::SHIFT_JIS_2004>(
  const char buffer[],
  std::string::size_type buffer_len,
  std::string::size_type start
)
{
  return next_seq_for_sjislike(buffer, buffer_len, start, "SHIFT_JIS_2004");
}

// https://en.wikipedia.org/wiki/Unified_Hangul_Code
template<> std::string::size_type next_seq<encoding_group::UHC>(
  const char buffer[],
  std::string::size_type buffer_len,
  std::string::size_type start
)
{
  if (start >= buffer_len)
    return std::string::npos;

  const auto first_byte = get_byte(buffer, start);
  if (first_byte < 0x80)
    return start + 1;

  if (start + 2 > buffer_len)
    throw_for_encoding_error("UHC", buffer, start, buffer_len - start);

  const auto second_byte = get_byte(buffer, start + 1);
  if (between_inc(first_byte, 0x80, 0xc6))
  {
    if (
      between_inc(second_byte, 0x41, 0x5a) or
      between_inc(second_byte, 0x61, 0x7a) or
      between_inc(second_byte, 0x80, 0xfe)
    )
      return start + 2;

    throw_for_encoding_error("UHC", buffer, start, 2);
  }

  if (between_inc(first_byte, 0xa1, 0xfe))
  {
    if (not between_inc(second_byte, 0xa1, 0xfe))
      throw_for_encoding_error("UHC", buffer, start, 2);

   return start + 2;
  }

  throw_for_encoding_error("UHC", buffer, start, 1);
}

// https://en.wikipedia.org/wiki/UTF-8#Description
template<> std::string::size_type next_seq<encoding_group::UTF8>(
  const char buffer[],
  std::string::size_type buffer_len,
  std::string::size_type start
)
{
  if (start >= buffer_len)
    return std::string::npos;

  const auto first_byte = get_byte(buffer, start);
  if (first_byte < 0x80)
    return start + 1;

  if (start + 2 > buffer_len)
      throw_for_encoding_error("UTF8", buffer, start, buffer_len - start);

  const auto second_byte = get_byte(buffer, start + 1);
  if (between_inc(first_byte, 0xc0, 0xdf))
  {
    if (not between_inc(second_byte, 0x80, 0xbf))
      throw_for_encoding_error("UTF8", buffer, start, 2);

    return start + 2;
  }

  if (start + 3 > buffer_len)
      throw_for_encoding_error("UTF8", buffer, start, buffer_len - start);

  const auto third_byte = get_byte(buffer, start + 2);
  if (between_inc(first_byte, 0xe0, 0xef))
  {
    if (
      between_inc(second_byte, 0x80, 0xbf) and
      between_inc(third_byte, 0x80, 0xbf)
    )
      return start + 3;

    throw_for_encoding_error("UTF8", buffer, start, 3);
  }

  if (start + 4 > buffer_len)
      throw_for_encoding_error("UTF8", buffer, start, buffer_len - start);

  if (between_inc(first_byte, 0xf0, 0xf7))
  {
    if (
      between_inc(second_byte, 0x80, 0xbf) and
      between_inc(third_byte, 0x80, 0xbf) and
      between_inc(get_byte(buffer, start + 3), 0x80, 0xbf)
    )
      return start + 4;

    throw_for_encoding_error("UTF8", buffer, start, 4);
  }

  throw_for_encoding_error("UTF8", buffer, start, 1);
}

} // namespace pqxx::internal
} // namespace pqxx


namespace pqxx
{
namespace internal
{

encoding_group enc_group(int libpq_enc_id)
{
  return enc_group(pg_encoding_to_char(libpq_enc_id));
}

encoding_group enc_group(const std::string& encoding_name)
{
  static const std::map<std::string, encoding_group> encoding_map{
    {"BIG5", encoding_group::BIG5},
    {"EUC_CN", encoding_group::EUC_CN},
    {"EUC_JP", encoding_group::EUC_JP},
    {"EUC_JIS_2004", encoding_group::EUC_JIS_2004},
    {"EUC_KR", encoding_group::EUC_KR},
    {"EUC_TW", encoding_group::EUC_TW},
    {"GB18030", encoding_group::GB18030},
    {"GBK", encoding_group::GBK},
    {"ISO_8859_5", encoding_group::MONOBYTE},
    {"ISO_8859_6", encoding_group::MONOBYTE},
    {"ISO_8859_7", encoding_group::MONOBYTE},
    {"ISO_8859_8", encoding_group::MONOBYTE},
    {"JOHAB", encoding_group::JOHAB},
    {"KOI8R", encoding_group::MONOBYTE},
    {"KOI8U", encoding_group::MONOBYTE},
    {"LATIN1", encoding_group::MONOBYTE},
    {"LATIN2", encoding_group::MONOBYTE},
    {"LATIN3", encoding_group::MONOBYTE},
    {"LATIN4", encoding_group::MONOBYTE},
    {"LATIN5", encoding_group::MONOBYTE},
    {"LATIN6", encoding_group::MONOBYTE},
    {"LATIN7", encoding_group::MONOBYTE},
    {"LATIN8", encoding_group::MONOBYTE},
    {"LATIN9", encoding_group::MONOBYTE},
    {"LATIN10", encoding_group::MONOBYTE},
    {"MULE_INTERNAL", encoding_group::MULE_INTERNAL},
    {"SJIS", encoding_group::SJIS},
    {"SHIFT_JIS_2004", encoding_group::SHIFT_JIS_2004},
    {"SQL_ASCII", encoding_group::MONOBYTE},
    {"UHC", encoding_group::UHC},
    {"UTF8", encoding_group::UTF8},
    {"WIN866", encoding_group::MONOBYTE},
    {"WIN874", encoding_group::MONOBYTE},
    {"WIN1250", encoding_group::MONOBYTE},
    {"WIN1251", encoding_group::MONOBYTE},
    {"WIN1252", encoding_group::MONOBYTE},
    {"WIN1253", encoding_group::MONOBYTE},
    {"WIN1254", encoding_group::MONOBYTE},
    {"WIN1255", encoding_group::MONOBYTE},
    {"WIN1256", encoding_group::MONOBYTE},
    {"WIN1257", encoding_group::MONOBYTE},
    {"WIN1258", encoding_group::MONOBYTE},
  };
  
  auto found_encoding_group{encoding_map.find(encoding_name)};
  if (found_encoding_group == encoding_map.end())
    throw std::invalid_argument{
      "unrecognized encoding '" + encoding_name + "'"
    };
  return found_encoding_group->second;
}

// Utility macro for implementing rutime-switched versions of templated encoding
// functions
#define DISPATCH_ENCODING_OPERATION(ENC, FUNCTION, ...) \
switch (ENC) \
{ \
case encoding_group::MONOBYTE: \
  return FUNCTION<encoding_group::MONOBYTE>(__VA_ARGS__); \
case encoding_group::BIG5: \
  return FUNCTION<encoding_group::BIG5>(__VA_ARGS__); \
case encoding_group::EUC_CN: \
  return FUNCTION<encoding_group::EUC_CN>(__VA_ARGS__); \
case encoding_group::EUC_JP: \
  return FUNCTION<encoding_group::EUC_JP>(__VA_ARGS__); \
case encoding_group::EUC_JIS_2004: \
  return FUNCTION<encoding_group::EUC_JIS_2004>(__VA_ARGS__); \
case encoding_group::EUC_KR: \
  return FUNCTION<encoding_group::EUC_KR>(__VA_ARGS__); \
case encoding_group::EUC_TW: \
  return FUNCTION<encoding_group::EUC_TW>(__VA_ARGS__); \
case encoding_group::GB18030: \
  return FUNCTION<encoding_group::GB18030>(__VA_ARGS__); \
case encoding_group::GBK: \
  return FUNCTION<encoding_group::GBK>(__VA_ARGS__); \
case encoding_group::JOHAB: \
  return FUNCTION<encoding_group::JOHAB>(__VA_ARGS__); \
case encoding_group::MULE_INTERNAL: \
  return FUNCTION<encoding_group::MULE_INTERNAL>(__VA_ARGS__); \
case encoding_group::SJIS: \
  return FUNCTION<encoding_group::SJIS>(__VA_ARGS__); \
case encoding_group::SHIFT_JIS_2004: \
  return FUNCTION<encoding_group::SHIFT_JIS_2004>(__VA_ARGS__); \
case encoding_group::UHC: \
  return FUNCTION<encoding_group::UHC>(__VA_ARGS__); \
case encoding_group::UTF8: \
  return FUNCTION<encoding_group::UTF8>(__VA_ARGS__); \
} \
throw pqxx::usage_error("Invalid encoding group code.")

std::string::size_type next_seq(
  encoding_group enc,
  const char buffer[],
  std::string::size_type buffer_len,
  std::string::size_type start
)
{
  DISPATCH_ENCODING_OPERATION(enc, next_seq, buffer, buffer_len, start);
}

std::string::size_type find_with_encoding(
  encoding_group enc,
  const std::string& haystack,
  char needle,
  std::string::size_type start
)
{
  DISPATCH_ENCODING_OPERATION(enc, find_with_encoding, haystack, needle, start);
}

std::string::size_type find_with_encoding(
  encoding_group enc,
  const std::string& haystack,
  const std::string& needle,
  std::string::size_type start
)
{
  DISPATCH_ENCODING_OPERATION(enc, find_with_encoding, haystack, needle, start);
}

#undef DISPATCH_ENCODING_OPERATION

} // namespace pqxx::internal
} // namespace pqxx
