#include <gtest/gtest.h>

#include "protocol/cli_formatter.hpp"
#include "protocol/resp_parser.hpp"
#include "protocol/resp_writer.hpp"
#include "util/string_utils.hpp"
TEST(Parser, EverySplitAndBinarySafe) {
  const std::vector<std::string> expected{"SET", std::string("a\0b", 3), "v\r\nx"};
  const auto bytes = mr::encode_command(expected);
  mr::RespParser parser;
  for (std::size_t split = 0; split < bytes.size(); ++split)
    EXPECT_EQ(parser.parse(std::string_view(bytes).substr(0, split)).status,
              mr::ParseStatus::Incomplete)
        << split;
  const auto result = parser.parse(bytes);
  EXPECT_EQ(result.status, mr::ParseStatus::Complete);
  EXPECT_EQ(result.args, expected);
  EXPECT_EQ(result.consumed, bytes.size());
}
TEST(Parser, PipelineConsumesOneRequest) {
  const auto first = mr::encode_command({"PING"});
  const auto bytes = first + mr::encode_command({"GET", "k"});
  mr::RespParser parser;
  const auto parsed = parser.parse(bytes);
  EXPECT_EQ(parsed.consumed, first.size());
  EXPECT_EQ(parser.parse(std::string_view(bytes).substr(parsed.consumed)).args,
            (std::vector<std::string>{"GET", "k"}));
}
TEST(Parser, InlineQuotesEscapesEmptyAndCase) {
  mr::RespParser parser;
  EXPECT_EQ(parser.parse("set \"hello world\" \"\"\r\n").args,
            (std::vector<std::string>{"set", "hello world", ""}));
  EXPECT_EQ(parser.parse("echo 'a\\nb'\n").args[1], "a\nb");
  EXPECT_EQ(parser.parse("set \"unterminated\n").status, mr::ParseStatus::ProtocolError);
}
TEST(Parser, RejectMalformedFraming) {
  mr::RespParser parser;
  for (const auto* bytes : {"*0\r\n", "*-1\r\n", "*wat\r\n", "*1\r\n+PING\r\n", "*1\r\n$-1\r\n",
                            "*1\r\n$1\r\naXX", "*1\r\n$99999999999999999999999999\r\n"})
    EXPECT_EQ(parser.parse(bytes).status, mr::ParseStatus::ProtocolError) << bytes;
}
TEST(Parser, EnforcesLimitsBeforeAllocatingPayload) {
  mr::ParserLimits limits;
  limits.max_bulk = 8;
  limits.max_array = 4;
  limits.max_inline = 8;
  limits.max_request = 32;
  mr::RespParser parser(limits);
  for (const auto* bytes : {"*1\r\n$9\r\n", "*5\r\n", "123456789", "123456789\n"})
    EXPECT_EQ(parser.parse(bytes).status, mr::ParseStatus::ProtocolError);
  EXPECT_EQ(parser.parse("*1\r\n$8\r\nabc").status, mr::ParseStatus::Incomplete);
}
TEST(Writer, AllReplyTypes) {
  EXPECT_EQ(mr::resp_encode(mr::Reply::ok()), "+OK\r\n");
  EXPECT_EQ(mr::resp_encode(mr::Reply::null()), "$-1\r\n");
  EXPECT_EQ(mr::resp_encode(mr::Reply::integer(-2)), ":-2\r\n");
  EXPECT_EQ(mr::resp_encode(mr::Reply::bulk(std::string("x\0", 2))),
            std::string("$2\r\nx\0\r\n", 8));
  EXPECT_EQ(mr::resp_encode(mr::Reply::array({mr::Reply::integer(1), mr::Reply::null()})),
            "*2\r\n:1\r\n$-1\r\n");
}
TEST(Writer, PreventsErrorReplyInjection) {
  EXPECT_EQ(mr::resp_encode(mr::Reply::error("ERR x\r\n+OK")), "-ERR x  +OK\r\n");
}
TEST(Formatter, HumanReplies) {
  EXPECT_EQ(mr::format_cli(mr::Reply::integer(42)), "(integer) 42");
  EXPECT_EQ(mr::format_cli(mr::Reply::null()), "(nil)");
}
TEST(StringUtils, GlobPatterns) {
  EXPECT_TRUE(mr::glob_match("h*[a-z]?", "hello"));
  EXPECT_TRUE(mr::glob_match("a\\*", "a*"));
  EXPECT_FALSE(mr::glob_match("a?", "a"));
  EXPECT_TRUE(mr::glob_match("[^a]", "b"));
  EXPECT_FALSE(mr::glob_match("[!a]", "a"));
}
TEST(StringUtils, CheckedIntegers) {
  EXPECT_FALSE(mr::parse_i64("1x"));
  EXPECT_FALSE(mr::parse_i64("9223372036854775808"));
  EXPECT_EQ(mr::parse_i64("-9223372036854775808"), std::numeric_limits<std::int64_t>::min());
}
