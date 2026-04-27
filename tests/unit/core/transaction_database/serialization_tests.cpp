#include <catch2/catch.hpp>

#include "core/transaction_database_core.h"

TEST_CASE("transaction database package token and item id are stable", "[unit][transaction_database][serialization]") {
    Package package;
    package.action = ActionType::INSTALL;
    package.system = "dnf";
    package.name = "ripgrep";
    package.version = "14.1";
    package.sourcePath = "/tmp/ripgrep.rpm";
    package.localTarget = true;

    CHECK(transaction_database_package_token(package) == "1|dnf|ripgrep|14.1|/tmp/ripgrep.rpm|1");

    const std::string itemId = transaction_database_item_id_for_package(package);
    CHECK(itemId == transaction_database_item_id_for_package(package));
    CHECK_FALSE(itemId.empty());
    CHECK(transaction_database_item_key("run-1", package) == transaction_database_item_prefix("run-1") + itemId);

    Package different = package;
    different.version = "14.2";
    CHECK(transaction_database_item_id_for_package(different) != itemId);
}

TEST_CASE("transaction database escapes and unescapes newline and backslash", "[unit][transaction_database][serialization]") {
    const std::string raw = std::string("line1\nline2\\path");
    const std::string escaped = transaction_database_escape_field(raw);

    CHECK(escaped == "line1\\nline2\\\\path");
    CHECK(transaction_database_unescape_field(escaped) == raw);
}

TEST_CASE("transaction database run serialization round-trips fields and flags", "[unit][transaction_database][serialization]") {
    TransactionRunRecord run;
    run.id = "run-42";
    run.state = "open\nstate";
    run.createdAt = "1000";
    run.updatedAt = "1001";
    run.flags = {std::string("--message=line1\nline2"), std::string("--path=C:\\temp")};

    const std::string payload = transaction_database_serialize_run(run);
    const std::optional<TransactionRunRecord> parsed = transaction_database_deserialize_run(run.id, payload);

    REQUIRE(parsed.has_value());
    CHECK(parsed->id == "run-42");
    CHECK(parsed->state == run.state);
    CHECK(parsed->createdAt == "1000");
    CHECK(parsed->updatedAt == "1001");
    CHECK(parsed->flags == run.flags);
    CHECK(payload.find("state=open\\nstate") != std::string::npos);
}

TEST_CASE("transaction database run deserializer rejects missing required fields", "[unit][transaction_database][serialization]") {
    CHECK_FALSE(transaction_database_deserialize_run("run-1", "createdAt=1000\nupdatedAt=1001\nflags=\n").has_value());
    CHECK_FALSE(transaction_database_deserialize_run("run-1", "state=open\nupdatedAt=1001\nflags=\n").has_value());
    CHECK_FALSE(transaction_database_deserialize_run("run-1", "state=open\ncreatedAt=1000\nflags=\n").has_value());
}

TEST_CASE("transaction database item serialization round-trips package payload", "[unit][transaction_database][serialization]") {
    TransactionItemRecord item;
    item.runId = "run-7";
    item.package.action = ActionType::REMOVE;
    item.package.system = "apt";
    item.package.name = "pkg\nname";
    item.package.version = "1.2\\beta";
    item.package.sourcePath = "/tmp/pkg\\name.deb";
    item.package.localTarget = false;
    item.status = "running";
    item.errorMessage = "bad\nthing";
    item.sequence = 3;
    item.itemId = transaction_database_item_id_for_package(item.package);

    const std::string key = transaction_database_item_key(item.runId, item.package);
    const std::string payload = transaction_database_serialize_item(item);
    const std::optional<TransactionItemRecord> parsed = transaction_database_deserialize_item(key, payload);

    REQUIRE(parsed.has_value());
    CHECK(parsed->runId == item.runId);
    CHECK(parsed->itemId == item.itemId);
    CHECK(parsed->sequence == 3);
    CHECK(parsed->package.action == ActionType::REMOVE);
    CHECK(parsed->package.system == "apt");
    CHECK(parsed->package.name == item.package.name);
    CHECK(parsed->package.version == item.package.version);
    CHECK(parsed->package.sourcePath == item.package.sourcePath);
    CHECK_FALSE(parsed->package.localTarget);
    CHECK(parsed->status == "running");
    CHECK(parsed->errorMessage == item.errorMessage);
}

TEST_CASE("transaction database item deserializer rejects malformed numeric and boolean fields", "[unit][transaction_database][serialization]") {
    const std::string key = "item:run-1:item-1";

    CHECK_FALSE(transaction_database_deserialize_item(
        key,
        "sequence=oops\naction=1\nsystem=dnf\nname=git\nversion=\nsourcePath=\nlocalTarget=0\nstatus=planned\nerror=\n"
    ).has_value());

    CHECK_FALSE(transaction_database_deserialize_item(
        key,
        "sequence=0\naction=oops\nsystem=dnf\nname=git\nversion=\nsourcePath=\nlocalTarget=0\nstatus=planned\nerror=\n"
    ).has_value());

    CHECK_FALSE(transaction_database_deserialize_item(
        key,
        "sequence=0\naction=1\nsystem=dnf\nname=git\nversion=\nsourcePath=\nlocalTarget=maybe\nstatus=planned\nerror=\n"
    ).has_value());
}

TEST_CASE("transaction database item deserializer rejects malformed key and missing required fields", "[unit][transaction_database][serialization]") {
    CHECK_FALSE(transaction_database_deserialize_item(
        "broken-key",
        "sequence=0\naction=1\nsystem=dnf\nname=git\nversion=\nsourcePath=\nlocalTarget=0\nstatus=planned\nerror=\n"
    ).has_value());

    CHECK_FALSE(transaction_database_deserialize_item(
        "item:run-1:item-1",
        "sequence=0\naction=1\nsystem=dnf\nname=git\nversion=\nsourcePath=\nlocalTarget=0\nerror=\n"
    ).has_value());

    CHECK_FALSE(transaction_database_deserialize_item(
        "item:run-1:item-1",
        "sequence=0\naction=1\nsystem=dnf\nname=git\nversion=\nlocalTarget=0\nstatus=planned\nerror=\n"
    ).has_value());
}
