// IREZ core tests: ports of the IREZ_V00_00 Python tests plus regression
// tests for the bugs fixed during the C++ port (B1-B12, docs/PROGRESS.md).
#include <cstdlib>
#include <algorithm>
#include <barrier>
#include <filesystem>
#include <fstream>
#include <future>
#include <regex>
#include <string>

#include <gtest/gtest.h>

#include <SQLiteCpp/Transaction.h>

#include "db.h"
#include "envelope.h"
#include "error.h"
#include "schema.h"
#include "service.h"
#include "store.h"
#include "util.h"

namespace {

const std::filesystem::path kFixtures = IREZ_FIXTURES_DIR;

struct TempState {
  std::filesystem::path dir;
  TempState() {
    dir = std::filesystem::temp_directory_path() /
          ("irez-test-" + irez::uuid4());
  }
  ~TempState() { std::filesystem::remove_all(dir); }
};

// A service bound to a fresh initialized state directory.
struct Fixture {
  TempState state;
  irez::Service service;
  Fixture() : service(state.dir) { service.init("test"); }

  // Ingest a fixture and return its artifact id.
  std::string ingest(const std::string &name, const std::string &index = "catalog") {
    auto response = service.ingest(kFixtures / name, index);
    return std::string(response.getObject("result")
                           ->getString("artifact")
                           .value_or(""));
  }

  std::string function_handle(const std::string &artifact, const std::string &key) {
    const std::string hash = artifact.substr(std::string("artifact:").size());
    return "irez:" + hash + ":llvm:function:" + key;
  }
};

TEST(Util, Uuid4Shape) {
  const std::string id = irez::uuid4();
  std::regex shape(
      "^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$");
  EXPECT_TRUE(std::regex_match(id, shape)) << id;
}

TEST(Util, Iso8601Shape) {
  const std::string ts = irez::now_iso8601();
  std::regex shape(
      "^\\d{4}-\\d{2}-\\d{2}T\\d{2}:\\d{2}:\\d{2}\\.\\d{6}\\+00:00$");
  EXPECT_TRUE(std::regex_match(ts, shape)) << ts;
}

TEST(Store, SchemaForeignKeysAndDedup) {
  TempState state;
  irez::Store store(state.dir);
  const std::string inv = store.initialize("test");
  EXPECT_TRUE(inv.starts_with("investigation:"));
  {
    SQLite::Database db = store.connect();
    auto [first, created] = store.ingest_file(db, kFixtures / "nonfloating.ll");
    auto [second, created_again] = store.ingest_file(db, kFixtures / "nonfloating.ll");
    EXPECT_TRUE(created);
    EXPECT_FALSE(created_again);
    EXPECT_EQ(first.at("id").as_string(), second.at("id").as_string());
    const auto fk = irez::query_one(db, "PRAGMA foreign_keys");
    EXPECT_EQ((*fk).at("foreign_keys").as_int(), 1);
    const auto version =
        irez::query_one(db, "SELECT value FROM schema_meta WHERE key='schema_version'");
    EXPECT_EQ((*version).at("value").as_string(), "2");
  }
}

TEST(Store, ReadConnectionDoesNotCreateMissingDatabase) {
  TempState state;
  irez::Store store(state.dir);
  EXPECT_THROW(store.connect(), irez::IrezError);
  EXPECT_FALSE(std::filesystem::exists(store.db_path()));
}

TEST(Store, FailedRunIsPersisted) {
  // B5 mechanism: a run row committed before the workload survives a rolled
  // back workload transaction and can be marked failed afterwards.
  TempState state;
  irez::Store store(state.dir);
  store.initialize("test");
  SQLite::Database db = store.connect();
  auto [artifact, created] = store.ingest_file(db, kFixtures / "nonfloating.ll");
  std::string run;
  {
    SQLite::Transaction transaction(db);
    run = store.run_start(db, "test-producer", artifact.at("id").as_string(), {"test"});
    transaction.commit();
  }
  try {
    SQLite::Transaction transaction(db);
    irez::exec(db, "UPDATE investigations SET name='changed'");
    throw std::runtime_error("boom");
  } catch (const std::exception &exc) {
    SQLite::Transaction failure(db);
    store.run_end(db, run, "failed", llvm::json::Object{{"message", exc.what()}});
    failure.commit();
  }
  const auto row = irez::query_one(db, "SELECT status,error_json FROM analysis_runs WHERE id=?",
                                   {run});
  ASSERT_TRUE(row.has_value());
  EXPECT_EQ((*row).at("status").as_string(), "failed");
  EXPECT_NE((*row).at("error_json").as_string().find("boom"), std::string::npos);
  // The failed workload was rolled back.
  EXPECT_EQ(store.investigation(db)->at("name").as_string(), "test");
}

TEST(Store, RejectsOldAndFutureSchemaWithoutMutation) {
  TempState state;
  std::filesystem::create_directories(state.dir);
  {
    SQLite::Database db((state.dir / "investigation.sqlite").string(),
                        SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
    db.exec(irez::kMigration1);
    irez::exec(db, "INSERT INTO schema_meta VALUES('schema_version','1')");
    irez::exec(db, "INSERT INTO investigations(id,name,created_at) VALUES(?,?,?)",
               {"investigation:test", "test", irez::now_iso8601()});
  }
  irez::Store store(state.dir);
  EXPECT_THROW(store.initialize("ignored"), irez::IrezError);
  {
    SQLite::Database db((state.dir / "investigation.sqlite").string(),
                        SQLite::OPEN_READWRITE);
    EXPECT_EQ(irez::query_one(
                  db, "SELECT value FROM schema_meta WHERE key='schema_version'")
                  ->at("value").as_string(),
              "1");
    irez::exec(db, "UPDATE schema_meta SET value='999' WHERE key='schema_version'");
  }
  EXPECT_THROW(store.initialize("ignored"), irez::IrezError);
}

TEST(Envelope, PartialAndTruncationAreExplicit) {
  irez::Envelope env;
  env.command = "slice";
  env.investigation = std::string("i");
  env.target = std::string("t");
  env.result = llvm::json::Object{};
  env.unknowns = llvm::json::Array{llvm::json::Object{{"x", 1}}};
  env.truncated = true;
  env.reason = "node_budget";
  env.visited = 2;
  env.budget = 2;
  const llvm::json::Object built = env.build();
  EXPECT_EQ(built.getInteger("schema_version").value_or(0), 1);
  EXPECT_TRUE(built.getObject("truncation")->getBoolean("truncated").value_or(false));
  EXPECT_EQ(built.getObject("truncation")->getString("reason").value_or(""),
            "node_budget");
  EXPECT_EQ(built.getArray("unknowns")->size(), 1u);
}

TEST(Envelope, InvalidUtf8IsReplacedAndMarked) {
  std::string invalid = "prefix";
  invalid.append("\xed\xa0\x80", 3);
  llvm::json::Object value{{"text", invalid}, {"diagnostics", llvm::json::Array{}}};
  const std::string serialized =
      irez::dump_json(llvm::json::Value(std::move(value)));
  auto parsed = irez::parse_json(serialized, "sanitized test output");
  const auto *object = parsed.getAsObject();
  ASSERT_TRUE(object != nullptr);
  EXPECT_TRUE(object->getBoolean("encoding_lossy").value_or(false));
  ASSERT_TRUE(object->getArray("diagnostics") != nullptr);
  EXPECT_FALSE(object->getArray("diagnostics")->empty());
}

TEST(Envelope, EveryByteValueStillProducesParseableUtf8Json) {
  std::string bytes;
  for (int value = 0; value <= 255; ++value)
    bytes.push_back(static_cast<char>(value));
  llvm::json::Object object{{"raw_bytes", bytes},
                            {"diagnostics", llvm::json::Array{}}};
  const std::string serialized =
      irez::dump_json(llvm::json::Value(std::move(object)));
  EXPECT_NO_THROW(irez::parse_json(serialized, "all-byte property case"));
  EXPECT_EQ(serialized.find('\0'), std::string::npos);
}

class ServiceFixture : public ::testing::Test {
protected:
  Fixture f;
};

TEST_F(ServiceFixture, CatalogLazyMaterializationAndQueries) {
  const std::string artifact = f.ingest("nonfloating.ll");
  const std::string fn = f.function_handle(artifact, "f1");

  auto functions = f.service.functions(artifact, std::optional<std::string>("choose"));
  ASSERT_EQ(functions.getArray("result")->size(), 1u);
  EXPECT_EQ((*functions.getArray("result"))[0]
                .getAsObject()
                ->getString("status")
                .value_or(""),
            "catalog_only");

  auto materialized = f.service.materialize(fn);
  EXPECT_GT(materialized.getObject("result")->getInteger("entities").value_or(0), 8);

  functions = f.service.functions(artifact, std::optional<std::string>("choose"));
  EXPECT_EQ((*functions.getArray("result"))[0]
                .getAsObject()
                ->getString("status")
                .value_or(""),
            "structure_ready");

  SQLite::Database db = f.service.store().connect();
  const auto add = irez::query_one(db, "SELECT id,attributes_json FROM entities WHERE opcode='add'");
  const auto phi = irez::query_one(db, "SELECT id FROM entities WHERE opcode='phi'");
  const auto ret = irez::query_one(db, "SELECT id FROM entities WHERE opcode='ret'");
  const auto call = irez::query_one(db, "SELECT id FROM entities WHERE opcode='call'");
  ASSERT_TRUE(add && phi && ret && call);
  const auto add_attributes =
      irez::parse_json((*add).at("attributes_json").as_string(), "attributes");
  EXPECT_TRUE(add_attributes.getAsObject()->getBoolean("nsw").value_or(false));
  const auto cfg_count = irez::query_one(
      db, "SELECT count(*) AS n FROM relations WHERE kind='llvm.cfg-successor'");
  EXPECT_EQ((*cfg_count).at("n").as_int(), 4);
  for (const auto &row : irez::query_all(
           db, "SELECT attributes_json FROM relations WHERE src_id=? AND kind='llvm.operand'",
           {(*phi).at("id").as_string()})) {
    const auto attributes = irez::parse_json(row.at("attributes_json").as_string(), "rel");
    EXPECT_TRUE(attributes.getAsObject()->get("incoming_block") != nullptr);
  }

  auto graph = f.service.graph((*ret).at("id").as_string(), "backward", 20, "json");
  EXPECT_FALSE(graph.getObject("truncation")->getBoolean("truncated").value_or(true));
  EXPECT_GT(graph.getObject("result")->getArray("nodes")->size(), 2u);
  EXPECT_EQ((*graph.getArray("capabilities_used"))[0]
                .getAsObject()
                ->getString("name")
                .value_or(""),
            "operand_graph");

  auto uses = f.service.uses((*add).at("id").as_string());
  EXPECT_EQ((*uses.getArray("capabilities_used"))[0]
                .getAsObject()
                ->getString("precision")
                .value_or(""),
            "exact");

  auto source = f.service.source((*add).at("id").as_string());
  ASSERT_GE(source.getArray("result")->size(), 1u);
  EXPECT_TRUE((*source.getArray("result"))[0]
                  .getAsObject()
                  ->getString("file")
                  .value_or("")
                  .ends_with("/fixtures/nonfloating.c"));

  auto context = f.service.context((*ret).at("id").as_string(), 20);
  std::set<std::string> refs;
  for (const auto &ref : *context.getArray("evidence_refs"))
    refs.insert(ref.getAsString()->str());
  EXPECT_EQ(refs.size(), context.getArray("evidence_refs")->size());
}

// B1: expanding an internal direct call must materialize the callee instead
// of reporting "external" (V00_00 read the relation kind through a duplicate
// column name and always took the external branch).
TEST_F(ServiceFixture, ExpandInternalCallMaterializesCallee) {
  const std::string artifact = f.ingest("internal_call.ll");
  const std::string caller = f.function_handle(artifact, "f1");
  const std::string helper = f.function_handle(artifact, "f0");
  f.service.materialize(caller);

  SQLite::Database db = f.service.store().connect();
  const auto call = irez::query_one(
      db, "SELECT id FROM entities WHERE opcode='call' AND function_id=?",
      {caller});
  ASSERT_TRUE(call.has_value());
  auto expansion = f.service.expand((*call).at("id").as_string());
  EXPECT_EQ(expansion.getString("command").value_or(""), "expand");
  EXPECT_EQ(expansion.getObject("result")->getString("status").value_or(""),
            "structure_ready");

  const auto state = irez::query_one(
      db, "SELECT status FROM materializations WHERE function_id=?", {helper});
  ASSERT_TRUE(state.has_value());
  EXPECT_EQ((*state).at("status").as_string(), "structure_ready");
}

// B1 (negative case): external calls still report effects unknown.
TEST_F(ServiceFixture, ExpandExternalCallStaysExternal) {
  const std::string artifact = f.ingest("nonfloating.ll");
  f.service.materialize(f.function_handle(artifact, "f1"));
  SQLite::Database db = f.service.store().connect();
  const auto call = irez::query_one(db, "SELECT id FROM entities WHERE opcode='call'");
  ASSERT_TRUE(call.has_value());
  auto expansion = f.service.expand((*call).at("id").as_string());
  EXPECT_EQ(expansion.getObject("result")->getString("status").value_or(""), "external");
  EXPECT_EQ(expansion.getObject("result")->getString("effects").value_or(""), "unknown");
}

// B2: slice on a catalog-only function must not silently return an empty
// graph; it materializes on demand like show() does. (A function handle has
// no outgoing operand edges by design, so the observable is the triggered
// materialization, not the edge count.)
TEST_F(ServiceFixture, SliceAutoMaterializesCatalogOnlyFunction) {
  const std::string artifact = f.ingest("internal_call.ll");
  const std::string helper = f.function_handle(artifact, "f0");
  f.service.slice(helper, "backward", {"operand"}, 100, 12);
  SQLite::Database db = f.service.store().connect();
  const auto state = irez::query_one(
      db, "SELECT status FROM materializations WHERE function_id=?", {helper});
  ASSERT_TRUE(state.has_value());
  EXPECT_EQ((*state).at("status").as_string(), "structure_ready");
  // And the materialized structure is queryable.
  const auto edges = irez::query_one(
      db, "SELECT count(*) AS n FROM relations WHERE function_id=?", {helper});
  EXPECT_GT((*edges).at("n").as_int(), 0);
}

// A missing completion marker is stale analysis, not an exact empty result.
// Querying it must refresh the function and restore explicit completion.
TEST_F(ServiceFixture, MissingCapabilityManifestTriggersAutomaticRefresh) {
  const std::string artifact = f.ingest("nonfloating.ll");
  f.service.materialize(f.function_handle(artifact, "f1"));
  {
    // Simulate a legacy state directory with no CD evidence.
    SQLite::Database db = f.service.store().connect();
    SQLite::Transaction txn(db);
    db.exec("DELETE FROM relations WHERE kind='llvm.control-dependence'");
    db.exec("DELETE FROM materialization_capabilities "
            "WHERE capability='control_dependence'");
    txn.commit();
  }
  SQLite::Database db = f.service.store().connect();
  const auto phi = irez::query_one(db, "SELECT id FROM entities WHERE opcode='phi'");
  ASSERT_TRUE(phi.has_value());
  auto guards = f.service.guards((*phi).at("id").as_string());
  const auto *caps = guards.getArray("capabilities_used");
  ASSERT_TRUE(caps != nullptr && !caps->empty());
  EXPECT_EQ((*caps)[0].getAsObject()->getString("status").value_or(""),
            "supported");
  EXPECT_TRUE(irez::query_one(
      db, "SELECT 1 AS ok FROM materialization_capabilities WHERE "
          "capability='control_dependence'").has_value());
}

// The diamond fixture: yes/no blocks are control-dependent on entry's br
// (edges ordinal 0/1); entry and merge post-dominate the branch and carry no
// control dependence.
TEST_F(ServiceFixture, ControlDependenceDiamondEdges) {
  const std::string artifact = f.ingest("nonfloating.ll");
  f.service.materialize(f.function_handle(artifact, "f1"));
  SQLite::Database db = f.service.store().connect();
  const auto edges = irez::query_all(
      db,
      "SELECT src_id,dst_id,ordinal,attributes_json FROM relations "
      "WHERE kind='llvm.control-dependence' ORDER BY src_id");
  ASSERT_EQ(edges.size(), 2u);
  auto ends_with = [](const std::string &value, const std::string &suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
  };
  EXPECT_TRUE(ends_with(edges[0].at("src_id").as_string(), ":block:f1:1"));
  EXPECT_TRUE(ends_with(edges[0].at("dst_id").as_string(), ":inst:f1:0"));
  EXPECT_EQ(edges[0].at("ordinal").as_int(), 0);
  EXPECT_TRUE(ends_with(edges[1].at("src_id").as_string(), ":block:f1:2"));
  EXPECT_EQ(edges[1].at("ordinal").as_int(), 1);
  const auto attrs =
      irez::parse_json(edges[0].at("attributes_json").as_string(), "attributes");
  const auto *attrs_obj = attrs.getAsObject();
  ASSERT_TRUE(attrs_obj != nullptr);
  EXPECT_EQ(attrs_obj->getString("successor").value_or(""), "block:f1:1");
}

TEST_F(ServiceFixture, ControlDependenceCoversMultiBlockBranchArm) {
  const std::string artifact = f.ingest("multiblock_branch.ll");
  f.service.materialize(f.function_handle(artifact, "f0"));
  SQLite::Database db = f.service.store().connect();
  const auto edge = irez::query_one(
      db, "SELECT count(*) AS n FROM relations WHERE "
          "kind='llvm.control-dependence' AND src_id LIKE '%:block:f0:2'");
  ASSERT_TRUE(edge.has_value());
  EXPECT_EQ(edge->at("n").as_int(), 1)
      << "then2 remains controlled by entry even though then1 does not "
         "post-dominate it";
}

TEST_F(ServiceFixture, ExactEmptyControlDependenceHasCompletionEvidence) {
  const std::string artifact = f.ingest("linear.ll");
  const std::string function = f.function_handle(artifact, "f0");
  f.service.materialize(function);
  SQLite::Database db = f.service.store().connect();
  EXPECT_EQ(irez::query_one(
                db, "SELECT count(*) AS n FROM relations WHERE "
                    "kind='llvm.control-dependence'")
                ->at("n").as_int(),
            0);
  EXPECT_TRUE(irez::query_one(
      db, "SELECT 1 AS ok FROM materialization_capabilities WHERE function_id=? "
          "AND capability='control_dependence' AND status='completed'",
      {function}).has_value());
}

TEST_F(ServiceFixture, SwitchControlEvidenceCarriesCaseAndPredicate) {
  const std::string artifact = f.ingest("switch.ll");
  f.service.materialize(f.function_handle(artifact, "f0"));
  SQLite::Database db = f.service.store().connect();
  const auto add = irez::query_one(
      db, "SELECT id FROM entities WHERE opcode='add' AND block_id LIKE '%:block:f0:2'");
  ASSERT_TRUE(add.has_value());
  auto guards = f.service.guards(add->at("id").as_string());
  ASSERT_EQ(guards.getArray("result")->size(), 1u);
  const auto *guard = (*guards.getArray("result"))[0].getAsObject();
  EXPECT_TRUE(guard->getString("predicate").value_or("").ends_with(":arg:f0:0"));
  EXPECT_EQ(guard->getObject("attributes")->getString("switch_case").value_or(""),
            "i32 1");
}

TEST_F(ServiceFixture, LoopsMultiExitInfiniteAndUnreachableMaterializeHonestly) {
  const std::string artifact = f.ingest("control_edges.ll", "full");
  SQLite::Database db = f.service.store().connect();
  EXPECT_EQ(irez::query_one(
                db, "SELECT count(*) AS n FROM materializations WHERE artifact_id=? "
                    "AND status='structure_ready'", {artifact})
                ->at("n").as_int(),
            3);
  EXPECT_EQ(irez::query_one(
                db, "SELECT count(*) AS n FROM materialization_capabilities mc "
                    "JOIN materializations m ON m.function_id=mc.function_id "
                    "WHERE m.artifact_id=? AND mc.capability='control_dependence' "
                    "AND mc.status='completed'", {artifact})
                ->at("n").as_int(),
            3);
}

// Exact guards: %a lives in the yes block, so its only guard is entry's br
// with required successor 0 and predicate %cond.
TEST_F(ServiceFixture, GuardsExactViaControlDependence) {
  const std::string artifact = f.ingest("nonfloating.ll");
  f.service.materialize(f.function_handle(artifact, "f1"));
  SQLite::Database db = f.service.store().connect();
  const auto add = irez::query_one(db, "SELECT id FROM entities WHERE opcode='add'");
  ASSERT_TRUE(add.has_value());
  auto guards = f.service.guards((*add).at("id").as_string());
  const auto *result = guards.getArray("result");
  ASSERT_TRUE(result != nullptr);
  ASSERT_EQ(result->size(), 1u);
  const auto *guard = (*result)[0].getAsObject();
  EXPECT_TRUE(guard->getString("terminator").value_or("").ends_with(":inst:f1:0"));
  EXPECT_EQ(guard->getInteger("required_successor").value_or(-1), 0);
  EXPECT_TRUE(guard->getString("predicate").value_or("").ends_with(":arg:f1:1"))
      << "predicate must be the %cond argument";
  const auto *caps = guards.getArray("capabilities_used");
  ASSERT_TRUE(caps != nullptr && !caps->empty());
  EXPECT_EQ((*caps)[0].getAsObject()->getString("precision").value_or(""),
            "exact");
  // The merge block (phi's home) post-dominates the branch: no guards.
  const auto phi = irez::query_one(db, "SELECT id FROM entities WHERE opcode='phi'");
  auto phi_guards = f.service.guards((*phi).at("id").as_string());
  EXPECT_TRUE(phi_guards.getArray("result") == nullptr ||
              phi_guards.getArray("result")->empty());
}

// A backward slice over control from the yes block must reach the entry
// branch through real edges and report no unsupported unknown. (CD edges are
// block-anchored: instruction-level "what controls me" is guards' job. Note
// merge itself is control-dependent on nothing — it post-dominates entry.)
TEST_F(ServiceFixture, SliceTraversesControlDependence) {
  const std::string artifact = f.ingest("nonfloating.ll");
  f.service.materialize(f.function_handle(artifact, "f1"));
  SQLite::Database db = f.service.store().connect();
  const auto yes = irez::query_one(
      db, "SELECT id FROM entities WHERE kind='basic_block' AND id LIKE '%:block:f1:1'");
  ASSERT_TRUE(yes.has_value());
  auto slice = f.service.slice((*yes).at("id").as_string(), "backward",
                               {"control"}, 100, 12);
  bool reached_branch = false;
  if (const auto *nodes = slice.getObject("result")->getArray("nodes"))
    for (const auto &node : *nodes) {
      const auto *object = node.getAsObject();
      if (object->getString("id").value_or("").ends_with(":inst:f1:0") &&
          object->getString("opcode").value_or("") == "br")
        reached_branch = true;
    }
  EXPECT_TRUE(reached_branch)
      << "control-dependence edges must connect the phi's block to entry's br";
  for (const auto &unknown : *slice.getArray("unknowns"))
    EXPECT_FALSE(unknown.getAsObject()
                     ->getString("relation")
                     .value_or("")
                     .contains("control-dependence"))
        << "fresh artifacts must not report control-dependence as unsupported";
}

// B4: reaching an already-known node at maximum depth is not a truncation
// boundary; the edge is real evidence. In the diamond fixture, %y is a direct
// operand of %a and also an operand of %b, so it is reached at maximum depth
// while already known: V00_00 reported %y as a boundary; only the genuinely
// unexplored constant may remain on the boundary list.
TEST_F(ServiceFixture, SliceKnownNodeAtMaxDepthIsNotABoundary) {
  const std::string artifact = f.ingest("diamond.ll", "full");
  const std::string hash = artifact.substr(std::string("artifact:").size());
  const std::string arg_y = "irez:" + hash + ":llvm:arg:f0:1";
  SQLite::Database db = f.service.store().connect();
  const auto ret = irez::query_one(db, "SELECT id FROM entities WHERE opcode='ret'");
  ASSERT_TRUE(ret.has_value());
  auto slice = f.service.slice((*ret).at("id").as_string(), "backward", {"operand"},
                               100, 2);
  bool y_on_boundary = false;
  bool constant_on_boundary = false;
  for (const auto &boundary : *slice.getArray("boundaries")) {
    const std::string text = boundary.getAsString()->str();
    y_on_boundary |= text == arg_y;
    constant_on_boundary |= text.find(":constant:") != std::string::npos;
  }
  EXPECT_FALSE(y_on_boundary) << "known node must not be a boundary";
  EXPECT_TRUE(constant_on_boundary) << "unexplored constant is a real boundary";
  // The shared edge into %y was reached at maximum depth and must be kept.
  bool found_shared_edge = false;
  for (const auto &edge : *slice.getObject("result")->getArray("relations")) {
    const auto *row = edge.getAsObject();
    if (row->getString("dst_id").value_or("") == arg_y &&
        row->getString("src_id").value_or("").find(":inst:") != std::string::npos)
      found_shared_edge = true;
  }
  EXPECT_TRUE(found_shared_edge);
}

// B6: globals are module-scoped; materializing functions that reference them
// must not assign a function_id.
TEST_F(ServiceFixture, GlobalsStayModuleScoped) {
  const std::string artifact = f.ingest("globals.ll");
  f.service.materialize(f.function_handle(artifact, "f0"));
  f.service.materialize(f.function_handle(artifact, "f1"));
  SQLite::Database db = f.service.store().connect();
  const auto global = irez::query_one(
      db, "SELECT function_id FROM entities WHERE kind='global'");
  ASSERT_TRUE(global.has_value());
  EXPECT_TRUE((*global).at("function_id").is_null());
}

// A3 (release checklist): slicing a module-scoped global returns the target
// node alone — and must say so in unknowns instead of letting
// nodes:1/relations:0 pass for "no relations exist". Locked in both
// directions and with a relation selection that excludes control-dependence
// (so the CD-capability unknown cannot mask a missing scope report).
TEST_F(ServiceFixture, GlobalSliceReportsScopeBoundary) {
  const std::string artifact = f.ingest("globals.ll", "full");
  const std::string hash = artifact.substr(std::string("artifact:").size());
  const std::string global = "irez:" + hash + ":llvm:global:g0";
  for (const std::string direction : {"forward", "backward"}) {
    auto slice = f.service.slice(global, direction,
                                 {"operand", "llvm.references-global"}, 50, 8);
    EXPECT_EQ(slice.getObject("result")->getArray("nodes")->size(), 1u)
        << direction;
    EXPECT_EQ(slice.getObject("result")->getArray("relations")->size(), 0u)
        << direction;
    const auto *unknowns = slice.getArray("unknowns");
    ASSERT_TRUE(unknowns != nullptr);
    bool scope_reported = false;
    for (const auto &entry : *unknowns) {
      const auto *object = entry.getAsObject();
      if (object && object->getString("kind").value_or("") == "scope")
        scope_reported = true;
    }
    EXPECT_TRUE(scope_reported)
        << direction << " slice of a global must report the scope boundary";
    EXPECT_FALSE(
        slice.getObject("truncation")->getBoolean("truncated").value_or(true))
        << direction;
  }
}

// B7: declarations are declaration_only, never fake structure_ready.
TEST_F(ServiceFixture, DeclarationsAreDeclarationOnly) {
  const std::string artifact = f.ingest("nonfloating.ll");
  auto functions = f.service.functions(artifact, std::optional<std::string>("external"));
  ASSERT_EQ(functions.getArray("result")->size(), 1u);
  EXPECT_EQ((*functions.getArray("result"))[0]
                .getAsObject()
                ->getString("status")
                .value_or(""),
            "declaration_only");
  auto response = f.service.materialize(f.function_handle(artifact, "f0"));
  EXPECT_EQ(response.getObject("result")->getString("status").value_or(""),
            "declaration_only");
}

// B8: re-ingesting identical content is a no-op beyond the lookup: no new
// analysis run, no entity churn.
TEST_F(ServiceFixture, DeduplicatedIngestDoesNoWork) {
  f.ingest("nonfloating.ll");
  SQLite::Database db = f.service.store().connect();
  const auto runs_before =
      irez::query_one(db, "SELECT count(*) AS n FROM analysis_runs");
  auto response = f.service.ingest(kFixtures / "nonfloating.ll", "catalog");
  EXPECT_TRUE(response.getObject("result")->getBoolean("deduplicated").value_or(false));
  const auto runs_after =
      irez::query_one(db, "SELECT count(*) AS n FROM analysis_runs");
  EXPECT_EQ((*runs_before).at("n").as_int(), (*runs_after).at("n").as_int());
}

TEST_F(ServiceFixture, DeduplicatedCatalogCanUpgradeToFull) {
  const std::string artifact = f.ingest("linear.ll", "catalog");
  auto response = f.service.ingest(kFixtures / "linear.ll", "full");
  EXPECT_TRUE(response.getObject("result")->getBoolean("deduplicated").value_or(false));
  SQLite::Database db = f.service.store().connect();
  EXPECT_EQ(irez::query_one(
                db, "SELECT status FROM materializations WHERE artifact_id=?",
                {artifact})
                ->at("status").as_string(),
            "structure_ready");
}

TEST_F(ServiceFixture, FailedCatalogStateIsRetried) {
  const std::string artifact = f.ingest("linear.ll");
  {
    SQLite::Database db = f.service.store().connect();
    irez::exec(db, "UPDATE artifacts SET catalog_status='failed' WHERE id=?",
               {artifact});
  }
  auto response = f.service.ingest(kFixtures / "linear.ll", "catalog");
  EXPECT_TRUE(response.getObject("result")->getBoolean("refreshed").value_or(false));
  SQLite::Database db = f.service.store().connect();
  EXPECT_EQ(irez::query_one(db, "SELECT catalog_status FROM artifacts WHERE id=?",
                            {artifact})->at("catalog_status").as_string(),
            "catalog_ready");
}

TEST_F(ServiceFixture, ConcurrentMaterializationHasSingleClaimProtocol) {
  const std::string artifact = f.ingest("multiblock_branch.ll");
  const std::string function = f.function_handle(artifact, "f0");
  std::barrier gate(3);
  auto worker = [&]() {
    irez::Service service(f.state.dir);
    gate.arrive_and_wait();
    try {
      auto result = service.materialize(function);
      return result.getObject("result")->getString("status").value_or("").str();
    } catch (const irez::IrezError &error) {
      return std::string(error.what());
    }
  };
  auto first = std::async(std::launch::async, worker);
  auto second = std::async(std::launch::async, worker);
  gate.arrive_and_wait();
  const std::string a = first.get();
  const std::string b = second.get();
  EXPECT_TRUE(a == "structure_ready" || b == "structure_ready");
  EXPECT_EQ(a.find("database is locked"), std::string::npos);
  EXPECT_EQ(b.find("database is locked"), std::string::npos);
  SQLite::Database db = f.service.store().connect();
  EXPECT_EQ(irez::query_one(db, "SELECT status FROM materializations WHERE function_id=?",
                            {function})->at("status").as_string(),
            "structure_ready");
}

TEST_F(ServiceFixture, ConcurrentIngestUsesContentAndCatalogClaims) {
  std::barrier gate(3);
  auto worker = [&]() {
    irez::Service service(f.state.dir);
    gate.arrive_and_wait();
    try {
      auto result = service.ingest(kFixtures / "linear.ll", "catalog");
      return result.getObject("result")->getString("artifact").value_or("").str();
    } catch (const irez::IrezError &error) {
      return std::string(error.what());
    }
  };
  auto first = std::async(std::launch::async, worker);
  auto second = std::async(std::launch::async, worker);
  gate.arrive_and_wait();
  const std::string a = first.get();
  const std::string b = second.get();
  EXPECT_TRUE(a.starts_with("artifact:") || b.starts_with("artifact:"));
  EXPECT_EQ(a.find("database is locked"), std::string::npos);
  EXPECT_EQ(b.find("database is locked"), std::string::npos);
  SQLite::Database db = f.service.store().connect();
  EXPECT_EQ(irez::query_one(db, "SELECT count(*) AS n FROM artifacts")
                ->at("n").as_int(), 1);
  EXPECT_EQ(irez::query_one(db, "SELECT catalog_status FROM artifacts")
                ->at("catalog_status").as_string(), "catalog_ready");
}

TEST_F(ServiceFixture, StaleMaterializationClaimIsRecoverable) {
  const std::string artifact = f.ingest("linear.ll");
  const std::string function = f.function_handle(artifact, "f0");
  {
    SQLite::Database db = f.service.store().connect();
    irez::exec(db,
               "UPDATE materializations SET status='materializing',claim_owner='dead',"
               "claim_started_at='2000-01-01T00:00:00+00:00' WHERE function_id=?",
               {function});
  }
  auto response = f.service.materialize(function);
  EXPECT_EQ(response.getObject("result")->getString("status").value_or(""),
            "structure_ready");
}

TEST_F(ServiceFixture, LiveMaterializationClaimReturnsStructuredBusy) {
  const std::string artifact = f.ingest("linear.ll");
  const std::string function = f.function_handle(artifact, "f0");
  {
    SQLite::Database db = f.service.store().connect();
    irez::exec(db,
               "UPDATE materializations SET status='materializing',claim_owner='live',"
               "claim_started_at=? WHERE function_id=?",
               {irez::now_iso8601(), function});
  }
  try {
    f.service.materialize(function);
    FAIL() << "expected live claim to reject duplicate materialization";
  } catch (const irez::Busy &error) {
    EXPECT_EQ(error.exit_code, 7);
    EXPECT_EQ(error.error_kind, "analysis_in_progress");
  }
}

// B9: ingest reports capabilities_used in the standard {name,status,precision}
// list shape, not as one nested dict.
TEST_F(ServiceFixture, IngestCapabilitiesAreFlattened) {
  const std::string artifact = f.ingest("nonfloating.ll");
  (void)artifact;
  auto response = f.service.ingest(kFixtures / "globals.ll", "catalog");
  const auto *caps = response.getArray("capabilities_used");
  ASSERT_TRUE(caps != nullptr);
  ASSERT_FALSE(caps->empty());
  for (const auto &cap : *caps)
    EXPECT_TRUE(cap.getAsObject()->getString("name").has_value());
}

TEST_F(ServiceFixture, FunctionShowIsBoundedAndTraceReturnIsComposed) {
  const std::string artifact = f.ingest("nonfloating.ll", "full");
  const std::string function = f.function_handle(artifact, "f1");
  auto summary = f.service.show(function);
  const auto *summary_result = summary.getObject("result");
  ASSERT_TRUE(summary_result != nullptr);
  EXPECT_EQ(summary_result->getString("handle").value_or(""), function);
  EXPECT_EQ(summary_result->getObject("counts")->getInteger("blocks").value_or(0), 4);
  EXPECT_TRUE(summary_result->get("exact_text") == nullptr);

  auto exact = f.service.show(function, "exact", std::nullopt, 100);
  EXPECT_TRUE(exact.getObject("result")->getString("exact_text").has_value());

  auto children = f.service.show(function, "children", "return", 1);
  const auto *items = children.getObject("result")->getArray("items");
  ASSERT_TRUE(items != nullptr);
  ASSERT_EQ(items->size(), 1u);
  EXPECT_EQ((*items)[0].getAsObject()->getString("opcode").value_or(""), "ret");
  EXPECT_TRUE((*items)[0].getAsObject()->getString("handle").has_value());

  auto trace = f.service.trace_return(function, "all", 20, 8);
  EXPECT_EQ(trace.getString("command").value_or(""), "trace-return");
  EXPECT_EQ(trace.getObject("result")->getInteger("return_count").value_or(0), 1);
  EXPECT_FALSE(trace.getObject("result")->getArray("sites")->empty());
  EXPECT_FALSE(trace.getArray("evidence_refs")->empty());
}

TEST_F(ServiceFixture, InvalidUtf8LlvmNameStillSerializesAsJson) {
  const std::string artifact = f.ingest("unicode_invalid_name.ll", "full");
  auto functions = f.service.functions(artifact, std::nullopt);
  const std::string serialized =
      irez::dump_json(llvm::json::Value(std::move(functions)));
  auto parsed = irez::parse_json(serialized, "unicode fixture response");
  const auto *object = parsed.getAsObject();
  ASSERT_TRUE(object != nullptr);
  EXPECT_TRUE(object->getBoolean("encoding_lossy").value_or(false));
}

TEST_F(ServiceFixture, MissingTargetIsExit3) {
  f.ingest("nonfloating.ll");
  try {
    f.service.show("irez:0000000000000000:llvm:inst:f9:9");
    FAIL() << "expected NotFound";
  } catch (const irez::NotFound &exc) {
    EXPECT_EQ(exc.exit_code, 3);
  }
}

// B12: suffix checks are case-insensitive for both the accept check and the
// stored media type.
TEST_F(ServiceFixture, UppercaseSuffixAccepted) {
  const std::filesystem::path copy =
      std::filesystem::temp_directory_path() / ("IREZ-TEST-" + irez::uuid4() + ".LL");
  std::filesystem::copy_file(kFixtures / "nonfloating.ll", copy);
  auto response = f.service.ingest(copy, "catalog");
  EXPECT_EQ(response.getObject("result")->getInteger("function_count").value_or(0), 2);
  SQLite::Database db = f.service.store().connect();
  const auto artifact = irez::query_one(db, "SELECT media_type FROM artifacts");
  EXPECT_EQ((*artifact).at("media_type").as_string(), "text/llvm");
  std::filesystem::remove(copy);
}

} // namespace

// Regression (V00_01 review): guards used LIMIT budget and always reported
// truncated=false, silently dropping guards beyond the budget. A zero budget
// returned an empty exact result with no truncation signal at all.
TEST_F(ServiceFixture, GuardsBudgetZeroReportsTruncationHonestly) {
  const std::string artifact = f.ingest("nonfloating.ll");
  f.service.materialize(f.function_handle(artifact, "f1"));
  SQLite::Database db = f.service.store().connect();
  const auto add = irez::query_one(db, "SELECT id FROM entities WHERE opcode='add'");
  ASSERT_TRUE(add.has_value());
  auto guards = f.service.guards((*add).at("id").as_string(), 0);
  const auto *truncation = guards.getObject("truncation");
  ASSERT_TRUE(truncation != nullptr);
  EXPECT_TRUE(truncation->getBoolean("truncated").value_or(false));
  EXPECT_EQ(truncation->getString("reason").value_or(""), "node_budget");
  EXPECT_TRUE(guards.getArray("result") == nullptr ||
              guards.getArray("result")->empty());
  const auto *boundaries = guards.getArray("boundaries");
  ASSERT_TRUE(boundaries != nullptr);
  ASSERT_EQ(boundaries->size(), 1u);
  EXPECT_TRUE((*boundaries)[0].getAsString()->ends_with(":inst:f1:0"))
      << "the dropped guard's terminator must stay reachable as a boundary";
}

// Same bug, positive budget: two guards competing for a budget of one must
// surface the overflow as truncation, not as a silently shorter exact list.
// (Fixtures carry at most one guard per block, so a second control edge is
// inserted directly.)
TEST_F(ServiceFixture, GuardsBeyondBudgetAreReportedAsBoundaries) {
  const std::string artifact = f.ingest("nonfloating.ll");
  f.service.materialize(f.function_handle(artifact, "f1"));
  SQLite::Database db = f.service.store().connect();
  const auto add = irez::query_one(db, "SELECT id FROM entities WHERE opcode='add'");
  ASSERT_TRUE(add.has_value());
  irez::exec(
      db,
      "INSERT INTO relations (artifact_id,function_id,src_id,dst_id,kind,ordinal,"
      "analysis_run_id,modality,precision,attributes_json) "
      "SELECT artifact_id,function_id,src_id,dst_id,kind,ordinal+10,"
      "analysis_run_id,modality,precision,attributes_json FROM relations "
      "WHERE kind='llvm.control-dependence'");
  auto guards = f.service.guards((*add).at("id").as_string(), 1);
  const auto *truncation = guards.getObject("truncation");
  ASSERT_TRUE(truncation != nullptr);
  EXPECT_TRUE(truncation->getBoolean("truncated").value_or(false));
  EXPECT_EQ(truncation->getString("reason").value_or(""), "node_budget");
  EXPECT_EQ(truncation->getInteger("visited_nodes").value_or(-1), 1);
  ASSERT_EQ(guards.getArray("result")->size(), 1u);
  ASSERT_EQ(guards.getArray("boundaries")->size(), 1u);
  // The untruncated query still returns both guards and no boundary.
  auto full = f.service.guards((*add).at("id").as_string(), 100);
  EXPECT_FALSE(full.getObject("truncation")->getBoolean("truncated").value_or(true));
  EXPECT_EQ(full.getArray("result")->size(), 2u);
  EXPECT_TRUE(full.getArray("boundaries")->empty());
}

// Regression (V00_01 review): a guards response item must expose decoded
// "attributes" and never leak the raw attributes_json storage column.
TEST_F(ServiceFixture, GuardsNeverLeakRawAttributesJson) {
  const std::string artifact = f.ingest("nonfloating.ll");
  f.service.materialize(f.function_handle(artifact, "f1"));
  SQLite::Database db = f.service.store().connect();
  const auto add = irez::query_one(db, "SELECT id FROM entities WHERE opcode='add'");
  ASSERT_TRUE(add.has_value());
  auto guards = f.service.guards((*add).at("id").as_string());
  const auto *result = guards.getArray("result");
  ASSERT_TRUE(result != nullptr && !result->empty());
  for (const auto &entry : *result) {
    const auto *guard = entry.getAsObject();
    ASSERT_TRUE(guard != nullptr);
    EXPECT_TRUE(guard->get("attributes_json") == nullptr)
        << "raw attributes_json must never reach the response";
    EXPECT_TRUE(guard->getObject("attributes") != nullptr);
  }
}

// Regression (V00_01 review): trace_return merged all per-site truncation
// into one top-level "per_return_budget", forcing callers to infer which
// return site hit which budget. Each site now carries its own truncation
// record and boundary list.
TEST_F(ServiceFixture, TraceReturnPreservesPerSiteTruncation) {
  const std::string artifact = f.ingest("nonfloating.ll", "full");
  const std::string function = f.function_handle(artifact, "f1");
  auto trace = f.service.trace_return(function, "all", 20, 1);
  const auto *sites = trace.getObject("result")->getArray("sites");
  ASSERT_TRUE(sites != nullptr);
  ASSERT_EQ(sites->size(), 1u);
  const auto *site = (*sites)[0].getAsObject();
  ASSERT_TRUE(site != nullptr);
  const auto *site_truncation = site->getObject("truncation");
  ASSERT_TRUE(site_truncation != nullptr);
  EXPECT_TRUE(site_truncation->getBoolean("truncated").value_or(false));
  EXPECT_EQ(site_truncation->getString("reason").value_or(""), "depth_budget");
  EXPECT_EQ(site_truncation->getInteger("budget_depth").value_or(-1), 1);
  EXPECT_EQ(site_truncation->getInteger("budget_nodes").value_or(-1), 20);
  EXPECT_GT(site_truncation->getInteger("visited_nodes").value_or(0), 0);
  const auto *site_boundaries = site->getArray("boundaries");
  ASSERT_TRUE(site_boundaries != nullptr);
  EXPECT_FALSE(site_boundaries->empty());
  // A single truncation reason across sites is promoted verbatim to the
  // top-level envelope instead of the vague per_return_budget label.
  const auto *truncation = trace.getObject("truncation");
  ASSERT_TRUE(truncation != nullptr);
  EXPECT_TRUE(truncation->getBoolean("truncated").value_or(false));
  EXPECT_EQ(truncation->getString("reason").value_or(""), "depth_budget");
  bool found_summary = false;
  for (const auto &diag : *trace.getArray("diagnostics")) {
    const auto *object = diag.getAsObject();
    if (object && object->getString("kind").value_or("") == "truncation") {
      found_summary = true;
      EXPECT_EQ(object->getString("scope").value_or(""), "per_sink");
      EXPECT_EQ(object->getInteger("truncated_sites").value_or(-1), 1);
    }
  }
  EXPECT_TRUE(found_summary) << "diagnostics must carry the per-return summary";
}

// Regression (V00_01 review): the materialization cache check compared a
// bare capability count (>= 6), so a foreign six-row manifest counted as
// current. The check now compares the exact contract capability set.
TEST_F(ServiceFixture, ForeignCapabilityManifestTriggersRefresh) {
  const std::string artifact = f.ingest("nonfloating.ll");
  const std::string function = f.function_handle(artifact, "f1");
  f.service.materialize(function);
  {
    SQLite::Database db = f.service.store().connect();
    irez::exec(db,
               "UPDATE materialization_capabilities SET capability='indirect_calls' "
               "WHERE function_id=? AND capability='direct_calls'",
               {function});
  }
  // A lazy query must re-validate the manifest and rematerialize.
  f.service.show(function);
  SQLite::Database db = f.service.store().connect();
  EXPECT_TRUE(irez::query_one(
      db, "SELECT 1 AS ok FROM materialization_capabilities WHERE function_id=? "
          "AND capability='direct_calls' AND status='completed'",
      {function}).has_value());
  EXPECT_FALSE(irez::query_one(
      db, "SELECT 1 AS ok FROM materialization_capabilities WHERE function_id=? "
          "AND capability='indirect_calls'",
      {function}).has_value());
}

// The contract capability set written by materialize must equal the
// generated contract constants exactly — same names, same precisions.
TEST_F(ServiceFixture, MaterializeWritesExactContractCapabilitySet) {
  const std::string artifact = f.ingest("nonfloating.ll");
  const std::string function = f.function_handle(artifact, "f1");
  f.service.materialize(function);
  SQLite::Database db = f.service.store().connect();
  const auto rows = irez::query_all(
      db, "SELECT capability,precision FROM materialization_capabilities "
          "WHERE function_id=? AND status='completed'",
      {function});
  ASSERT_EQ(rows.size(), irez::kMaterializationCapabilityCount);
  for (const auto &expected : irez::kMaterializationCapabilities) {
    const bool found = std::any_of(rows.begin(), rows.end(), [&](const irez::Row &row) {
      return row.at("capability").as_string() == expected.name &&
             row.at("precision").as_string() == expected.precision;
    });
    EXPECT_TRUE(found) << "missing contract capability: " << expected.name;
  }
}

// Projection contract (V00_01 round 3): detail=summary replaces the graph
// payload with a chain spine plus counts and value_shape, while keeping the
// correctness contract (per-site truncation) and the call/source sections.
TEST_F(ServiceFixture, TraceReturnSummaryOmitsGraphAndKeepsContract) {
  const std::string artifact = f.ingest("nonfloating.ll", "full");
  const std::string function = f.function_handle(artifact, "f1");
  auto trace = f.service.trace_return(function, "all", 20, 8, "summary");
  const auto *sites = trace.getObject("result")->getArray("sites");
  ASSERT_TRUE(sites != nullptr);
  ASSERT_EQ(sites->size(), 1u);
  const auto *site = (*sites)[0].getAsObject();
  ASSERT_TRUE(site != nullptr);
  EXPECT_TRUE(site->get("value_graph") == nullptr)
      << "summary detail must not emit the graph payload";
  const auto *chain = site->getArray("chain");
  ASSERT_TRUE(chain != nullptr);
  EXPECT_FALSE(chain->empty());
  EXPECT_GT(site->getInteger("node_count").value_or(0), 0);
  EXPECT_GT(site->getInteger("relation_count").value_or(0), 0);
  EXPECT_EQ(site->getString("value_shape").value_or(""), "scalar");
  // Per-site truncation is a correctness contract: present in every detail.
  ASSERT_TRUE(site->getObject("truncation") != nullptr);
  // Calls and boundaries stay in the summary, and boundary targets are named
  // so callers need no follow-up show to identify the callee.
  const auto *boundaries = site->getArray("call_boundaries");
  ASSERT_TRUE(boundaries != nullptr);
  ASSERT_FALSE(boundaries->empty());
  const auto *boundary = (*boundaries)[0].getAsObject();
  ASSERT_TRUE(boundary != nullptr);
  EXPECT_EQ(boundary->getString("target_name").value_or(""), "external");
  EXPECT_TRUE(boundary->getString("target_llvm_type").has_value());
  EXPECT_EQ(boundary->getString("status").value_or(""), "external");
  EXPECT_FALSE(site->getArray("direct_calls")->empty());
  EXPECT_TRUE(site->get("source") != nullptr);
  // Diagnostics echo the projection actually applied.
  const auto *diagnostics = trace.getArray("diagnostics");
  ASSERT_TRUE(diagnostics != nullptr);
  const auto *composed = (*diagnostics)[0].getAsObject();
  ASSERT_TRUE(composed != nullptr);
  EXPECT_EQ(composed->getString("detail").value_or(""), "summary");
  ASSERT_TRUE(composed->getArray("sections") != nullptr);
}

// Backlog F5: include EXTENDS the detail default instead of replacing it —
// the documented "add flags on top" semantics. Under detail=summary, asking
// for the nodes section keeps the summary sections (chain, calls, source)
// and adds value_graph.nodes; relations stay out because nothing requested
// them.
TEST_F(ServiceFixture, TraceReturnIncludeExtendsDetailDefault) {
  const std::string artifact = f.ingest("nonfloating.ll", "full");
  const std::string function = f.function_handle(artifact, "f1");
  auto trace = f.service.trace_return(function, "all", 20, 8, "summary",
                                      {"nodes"});
  const auto *site = (*trace.getObject("result")->getArray("sites"))[0].getAsObject();
  ASSERT_TRUE(site != nullptr);
  ASSERT_TRUE(site->getObject("value_graph") != nullptr);
  EXPECT_FALSE(site->getObject("value_graph")->getArray("nodes")->empty());
  EXPECT_TRUE(site->getObject("value_graph")->get("relations") == nullptr)
      << "relations were neither in the summary default nor requested";
  EXPECT_TRUE(site->getArray("chain") != nullptr);
  EXPECT_TRUE(site->getArray("call_boundaries") != nullptr)
      << "summary-default sections survive an explicit include";
  EXPECT_TRUE(site->get("source") != nullptr);
  ASSERT_TRUE(site->getObject("truncation") != nullptr)
      << "truncation survives every projection";
}

// The F5 reproduction: detail=graph + include=["flags"] previously REPLACED
// the graph projection with flags alone, silently dropping nodes/relations.
// Additive semantics keep the graph and add the sparse flags rows.
TEST_F(ServiceFixture, TraceReturnIncludeFlagsKeepsGraphProjection) {
  const std::string artifact = f.ingest("nonfloating.ll", "full");
  const std::string function = f.function_handle(artifact, "f1");
  auto trace = f.service.trace_return(function, "all", 20, 8, "graph",
                                      {"flags"});
  const auto *site = (*trace.getObject("result")->getArray("sites"))[0].getAsObject();
  ASSERT_TRUE(site != nullptr);
  const auto *graph = site->getObject("value_graph");
  ASSERT_TRUE(graph != nullptr) << "include must not drop the graph projection";
  EXPECT_FALSE(graph->getArray("nodes")->empty());
  EXPECT_TRUE(graph->getArray("relations") != nullptr);
  const auto *flags = site->getArray("flags");
  ASSERT_TRUE(flags != nullptr);
  EXPECT_EQ(flags->size(), 2u) << "the nsw add and sub carry flags";
}

// Unknown include sections and unknown detail values are usage errors
// (exit 2), never a silent fallback to the full graph.
TEST_F(ServiceFixture, TraceReturnRejectsUnknownProjection) {
  const std::string artifact = f.ingest("nonfloating.ll", "full");
  const std::string function = f.function_handle(artifact, "f1");
  try {
    (void)f.service.trace_return(function, "all", 20, 8, "graph", {"bogus"});
    FAIL() << "expected IrezError for unknown include section";
  } catch (const irez::IrezError &exc) {
    EXPECT_EQ(exc.exit_code, 2);
  }
  try {
    (void)f.service.trace_return(function, "all", 20, 8, "compact");
    FAIL() << "expected IrezError for unknown detail";
  } catch (const irez::IrezError &exc) {
    EXPECT_EQ(exc.exit_code, 2);
  }
}

// Summary is the default detail: the graph payload is opt-in, requested
// explicitly with detail=graph, which keeps the legacy payload shape.
TEST_F(ServiceFixture, TraceReturnSummaryIsDefaultGraphIsOptIn) {
  const std::string artifact = f.ingest("nonfloating.ll", "full");
  const std::string function = f.function_handle(artifact, "f1");
  auto trace = f.service.trace_return(function, "all", 20, 8);
  const auto *site = (*trace.getObject("result")->getArray("sites"))[0].getAsObject();
  ASSERT_TRUE(site != nullptr);
  EXPECT_TRUE(site->get("value_graph") == nullptr)
      << "default detail must be summary (no graph payload)";
  EXPECT_TRUE(site->getArray("chain") != nullptr);
  const auto *composed = (*trace.getArray("diagnostics"))[0].getAsObject();
  EXPECT_EQ(composed->getString("detail").value_or(""), "summary");

  auto graph_trace = f.service.trace_return(function, "all", 20, 8, "graph");
  const auto *graph_site =
      (*graph_trace.getObject("result")->getArray("sites"))[0].getAsObject();
  ASSERT_TRUE(graph_site != nullptr);
  const auto *graph = graph_site->getObject("value_graph");
  ASSERT_TRUE(graph != nullptr);
  EXPECT_FALSE(graph->getArray("nodes")->empty());
  EXPECT_TRUE(graph->getArray("relations") != nullptr);
  EXPECT_TRUE(graph_site->get("chain") == nullptr);
  EXPECT_GT(graph_site->getInteger("node_count").value_or(0), 0);
  EXPECT_EQ(graph_site->getString("value_shape").value_or(""), "scalar");
  const auto *graph_composed =
      (*graph_trace.getArray("diagnostics"))[0].getAsObject();
  EXPECT_EQ(graph_composed->getString("detail").value_or(""), "graph");
}

// Opt-in flags section: nodes carrying non-default boolean instruction
// attributes appear as sparse {handle, opcode, flags} rows. nonfloating's
// add/sub are nsw, so both must show up; the ret/call/argument nodes carry
// no true boolean attribute and stay out of the list.
TEST_F(ServiceFixture, TraceReturnFlagsSectionListsOnlyNonDefaultFlags) {
  const std::string artifact = f.ingest("nonfloating.ll", "full");
  const std::string function = f.function_handle(artifact, "f1");
  auto trace = f.service.trace_return(function, "all", 20, 8, "summary",
                                      {"chain", "flags"});
  const auto *site = (*trace.getObject("result")->getArray("sites"))[0].getAsObject();
  ASSERT_TRUE(site != nullptr);
  const auto *flags = site->getArray("flags");
  ASSERT_TRUE(flags != nullptr) << "requested flags section must be present";
  ASSERT_EQ(flags->size(), 2u) << "exactly the nsw add and sub carry flags";
  for (const auto &entry : *flags) {
    const auto *row = entry.getAsObject();
    ASSERT_TRUE(row != nullptr);
    EXPECT_TRUE(row->getString("handle").has_value());
    const std::string opcode = row->getString("opcode").value_or("").str();
    EXPECT_TRUE(opcode == "add" || opcode == "sub");
    const auto *active = row->getObject("flags");
    ASSERT_TRUE(active != nullptr);
    EXPECT_TRUE(active->getBoolean("nsw").value_or(false));
  }
  // flags is never part of a default projection.
  auto plain = f.service.trace_return(function, "all", 20, 8);
  const auto *plain_site =
      (*plain.getObject("result")->getArray("sites"))[0].getAsObject();
  EXPECT_TRUE(plain_site->get("flags") == nullptr);
}

// A requested flags section with no flagged nodes returns an empty array,
// so "checked, none set" is distinguishable from "section not requested".
TEST_F(ServiceFixture, TraceReturnFlagsSectionEmptyMeansCheckedNoneSet) {
  const std::string artifact = f.ingest("internal_call.ll", "full");
  const std::string function = f.function_handle(artifact, "f1"); // caller
  auto trace = f.service.trace_return(function, "all", 20, 8, "summary",
                                      {"chain", "flags"});
  const auto *site = (*trace.getObject("result")->getArray("sites"))[0].getAsObject();
  ASSERT_TRUE(site != nullptr);
  const auto *flags = site->getArray("flags");
  ASSERT_TRUE(flags != nullptr);
  EXPECT_TRUE(flags->empty())
      << "caller's chain carries no true boolean instruction attribute";
}

// Status must be self-describing: the resolved state directory is reported
// so a wrong --state-dir/IREZ_STATE_DIR is visible in the first response
// (fld1 rerun misdiagnosis root cause).
TEST_F(ServiceFixture, StatusReportsResolvedStateDir) {
  f.ingest("nonfloating.ll");
  auto status = f.service.status();
  const auto *result = status.getObject("result");
  ASSERT_TRUE(result != nullptr);
  const std::string reported = result->getString("state_dir").value_or("").str();
  EXPECT_FALSE(reported.empty());
  EXPECT_TRUE(std::filesystem::path(reported).is_absolute());
  EXPECT_EQ(reported.find('\\'), std::string::npos)
      << "state_dir uses forward slashes on every OS";
  const std::string expected = std::filesystem::weakly_canonical(
      std::filesystem::absolute(f.state.dir)).generic_string();
  EXPECT_EQ(reported, expected);
}

// Call-boundary honesty: a llvm.calls edge whose target has no entity row is
// "unknown", never "internal"+expandable; a declaration-only target explains
// itself via reason. (The boundary loop is exercised through trace_return.)
TEST_F(ServiceFixture, TraceReturnCallBoundaryReasonAndUnknownTarget) {
  const std::string artifact = f.ingest("nonfloating.ll", "full");
  const std::string function = f.function_handle(artifact, "f1");
  SQLite::Database db = f.service.store().connect();
  const auto call = irez::query_one(
      db, "SELECT id FROM entities WHERE opcode='call' AND function_id=?",
      {function});
  ASSERT_TRUE(call.has_value());
  const std::string ghost =
      "irez:0000000000000000:llvm:function:f9"; // no entity row
  // FK enforcement is per-connection; trace_return queries through its own
  // connection, so the dangling edge persists for the query.
  irez::exec(db, "PRAGMA foreign_keys=OFF");
  irez::exec(db,
             "INSERT INTO relations (artifact_id,function_id,src_id,dst_id,kind,"
             "ordinal,modality,precision,attributes_json) "
             "SELECT artifact_id,function_id,src_id,?,'llvm.calls',ordinal+10,"
             "modality,precision,attributes_json FROM relations "
             "WHERE src_id=? AND kind='llvm.calls'",
             {ghost, (*call).at("id").as_string()});
  auto trace = f.service.trace_return(function, "all", 20, 8);
  const auto *site = (*trace.getObject("result")->getArray("sites"))[0].getAsObject();
  ASSERT_TRUE(site != nullptr);
  const auto *boundaries = site->getArray("call_boundaries");
  ASSERT_TRUE(boundaries != nullptr);
  ASSERT_EQ(boundaries->size(), 2u);
  bool saw_external = false, saw_unknown = false;
  for (const auto &entry : *boundaries) {
    const auto *boundary = entry.getAsObject();
    ASSERT_TRUE(boundary != nullptr);
    const std::string status = boundary->getString("status").value_or("").str();
    const std::string reason = boundary->getString("reason").value_or("").str();
    if (status == "external") {
      saw_external = true;
      EXPECT_EQ(reason, "declaration_only");
      EXPECT_EQ(boundary->getString("target_name").value_or(""), "external");
      EXPECT_FALSE(boundary->getBoolean("expandable").value_or(true));
    } else if (status == "unknown") {
      saw_unknown = true;
      EXPECT_EQ(reason, "target_not_indexed");
      EXPECT_EQ(boundary->getString("target").value_or(""), ghost);
      EXPECT_FALSE(boundary->getBoolean("expandable").value_or(true));
    } else {
      FAIL() << "unexpected boundary status: " << status;
    }
  }
  EXPECT_TRUE(saw_external);
  EXPECT_TRUE(saw_unknown) << "unindexed target must surface as unknown";
}

// Backlog F2: trace-stores reaches the store sink of a kernel-shaped
// function. store_kernel's divide_bitcast_fusion ends in `ret ptr null`; the
// fmul -> fsub -> fdiv -> store chain is only visible through the store.
TEST_F(ServiceFixture, TraceStoresReachesStoreSinkChain) {
  const std::string artifact = f.ingest("store_kernel.ll", "full");
  const std::string function = f.function_handle(artifact, "f0");
  auto trace = f.service.trace_stores(function, 20, 8);
  const auto *result = trace.getObject("result");
  ASSERT_TRUE(result != nullptr);
  EXPECT_EQ(result->getInteger("store_count").value_or(0), 1);
  const auto *sites = result->getArray("sites");
  ASSERT_TRUE(sites != nullptr);
  ASSERT_EQ(sites->size(), 1u);
  const auto *site = (*sites)[0].getAsObject();
  ASSERT_TRUE(site != nullptr);
  EXPECT_EQ(site->getObject("store")->getString("opcode").value_or(""), "store");
  const auto *value = site->getObject("value");
  ASSERT_TRUE(value != nullptr);
  EXPECT_EQ(value->getString("opcode").value_or(""), "fdiv")
      << "the stored value is the fdiv result";
  EXPECT_EQ(site->getString("value_shape").value_or(""), "scalar");
  const auto *pointer = site->getObject("pointer");
  ASSERT_TRUE(pointer != nullptr);
  EXPECT_EQ(pointer->getString("name").value_or(""), "out")
      << "the destination pointer is the %out argument";
  const auto *chain = site->getArray("chain");
  ASSERT_TRUE(chain != nullptr);
  bool saw_fdiv = false, saw_fmul = false;
  for (const auto &entry : *chain) {
    const std::string opcode =
        entry.getAsObject()->getString("opcode").value_or("").str();
    saw_fdiv = saw_fdiv || opcode == "fdiv";
    saw_fmul = saw_fmul || opcode == "fmul";
  }
  EXPECT_TRUE(saw_fdiv && saw_fmul)
      << "the backward chain from the store reaches the arithmetic spine";
  ASSERT_TRUE(site->getObject("truncation") != nullptr)
      << "per-site truncation is the correctness contract";
  const auto *composed = (*trace.getArray("diagnostics"))[0].getAsObject();
  ASSERT_TRUE(composed != nullptr);
  EXPECT_EQ(composed->getString("sink").value_or(""), "store");
}

// A function without stores reports an honest empty result, not an error.
TEST_F(ServiceFixture, TraceStoresEmptyMeansNoStoreInstructions) {
  const std::string artifact = f.ingest("nonfloating.ll", "full");
  const std::string function = f.function_handle(artifact, "f1");
  auto trace = f.service.trace_stores(function, 20, 8);
  EXPECT_EQ(trace.getObject("result")->getInteger("store_count").value_or(-1), 0);
  const auto *unknowns = trace.getArray("unknowns");
  ASSERT_TRUE(unknowns != nullptr);
  bool saw_none = false;
  for (const auto &entry : *unknowns)
    if (entry.getAsObject()->getString("kind").value_or("") == "store_sites")
      saw_none = true;
  EXPECT_TRUE(saw_none) << "no-store functions must say so in unknowns";
}

// Backlog F2 companion: trace_return on the kernel shape flags the store
// result channel in unknowns; a normal value-returning function stays silent.
TEST_F(ServiceFixture, TraceReturnHintsStoreChannelForKernelShape) {
  const std::string artifact = f.ingest("store_kernel.ll", "full");
  const std::string function = f.function_handle(artifact, "f0");
  auto trace = f.service.trace_return(function, "all", 20, 8);
  bool saw_hint = false;
  for (const auto &entry : *trace.getArray("unknowns")) {
    const auto *unknown = entry.getAsObject();
    if (unknown->getString("kind").value_or("") == "result_channel") {
      saw_hint = true;
      EXPECT_EQ(unknown->getString("hint").value_or(""),
                "use trace-stores to trace the stored values");
    }
  }
  EXPECT_TRUE(saw_hint) << "ret-null + store must point at trace-stores";

  const std::string plain_artifact = f.ingest("nonfloating.ll", "full");
  const std::string plain = f.function_handle(plain_artifact, "f1");
  auto plain_trace = f.service.trace_return(plain, "all", 20, 8);
  for (const auto &entry : *plain_trace.getArray("unknowns"))
    EXPECT_NE(entry.getAsObject()->getString("kind").value_or(""),
              "result_channel")
        << "a real value return must not raise the store-channel hint";
}

// Backlog F4: an empty functions result with an implicitly defaulted target
// explains the applied scope; an explicit artifact or a non-empty result
// stays quiet.
TEST_F(ServiceFixture, FunctionsEmptyResultExplainsImplicitTarget) {
  const std::string artifact = f.ingest("nonfloating.ll");
  auto empty = f.service.functions(std::nullopt, std::string("no_such_name"));
  ASSERT_TRUE(empty.getArray("result") != nullptr);
  EXPECT_TRUE(empty.getArray("result")->empty());
  const auto *diagnostics = empty.getArray("diagnostics");
  ASSERT_TRUE(diagnostics != nullptr);
  ASSERT_FALSE(diagnostics->empty());
  EXPECT_EQ((*diagnostics)[0].getAsObject()->getString("kind").value_or(""),
            "implicit_target");

  auto explicit_artifact =
      f.service.functions(artifact, std::string("no_such_name"));
  EXPECT_TRUE(explicit_artifact.getArray("diagnostics")->empty())
      << "an explicit artifact scope needs no implicit-target note";

  auto nonempty = f.service.functions(std::nullopt, std::nullopt);
  EXPECT_TRUE(nonempty.getArray("diagnostics")->empty());
}

// Backlog F6: status documents what materialized_functions actually counts,
// so the storage-level figure is not misread as query-level readiness. (The
// environment-dependent state_dir_conflict warning is covered by the golden
// test with normalization, not here.)
TEST_F(ServiceFixture, StatusDocumentsMaterializationCountSemantics) {
  f.ingest("nonfloating.ll");
  auto status = f.service.status();
  const auto *diagnostics = status.getArray("diagnostics");
  ASSERT_TRUE(diagnostics != nullptr);
  bool saw_semantics = false;
  for (const auto &entry : *diagnostics)
    if (entry.getAsObject()->getString("kind").value_or("") ==
        "materialization_counts")
      saw_semantics = true;
  EXPECT_TRUE(saw_semantics);
}
