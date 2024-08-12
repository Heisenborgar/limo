#include "../src/core/casematchingdeployer.h"
#include "../src/core/deployer.h"
#include "test_utils.h"
#include <catch2/catch_test_macros.hpp>


TEST_CASE("Mods are added and removed", "[.deployer]")
{
  Deployer depl = Deployer(DATA_DIR / "source", DATA_DIR / "app", "");
  depl.addProfile();
  depl.addMod(2, true);
  REQUIRE(depl.getNumMods() == 1);
  depl.removeMod(2);
  REQUIRE(depl.getNumMods() == 0);
}

TEST_CASE("Mods are being deployed", "[.deployer]")
{
  resetAppDir();
  Deployer depl = Deployer(DATA_DIR / "source", DATA_DIR / "app", "");
  depl.addProfile();
  depl.addMod(1, true);
  depl.deploy();
  verifyDirsAreEqual(DATA_DIR / "app", DATA_DIR / "target" / "mod1", true);
}

TEST_CASE("Mod status works", "[.deployer]")
{
  resetAppDir();
  Deployer depl = Deployer(DATA_DIR / "source", DATA_DIR / "app", "");
  depl.addProfile();
  depl.addMod(1, false);
  depl.setModStatus(1, true);
  depl.addMod(0, true);
  depl.setModStatus(0, false);
  depl.deploy();
  verifyDirsAreEqual(DATA_DIR / "app", DATA_DIR / "target" / "mod1", true);
}

TEST_CASE("Deployed mods are removed", "[.deployer]")
{
  resetAppDir();
  Deployer depl = Deployer(DATA_DIR / "source", DATA_DIR / "app", "");
  depl.addProfile();
  depl.addMod(1, true);
  depl.deploy();
  depl.setModStatus(1, false);
  depl.deploy();
  verifyDirsAreEqual(DATA_DIR / "app", DATA_DIR / "source" / "app", true);
}

TEST_CASE("Conflicts are resolved", "[.deployer]")
{
  resetAppDir();
  Deployer depl = Deployer(DATA_DIR / "source", DATA_DIR / "app", "");
  depl.addProfile();
  depl.addMod(0, true);
  depl.addMod(1, true);
  depl.addMod(2, true);
  depl.deploy();
  verifyDirsAreEqual(DATA_DIR / "app", DATA_DIR / "target" / "mod012", true);
}

TEST_CASE("Files are restored", "[.deployer]")
{
  resetAppDir();
  Deployer depl = Deployer(DATA_DIR / "source", DATA_DIR / "app", "");
  depl.addProfile();
  depl.addMod(0, true);
  depl.addMod(1, true);
  depl.addMod(2, true);
  depl.deploy();
  depl.setModStatus(0, false);
  depl.setModStatus(1, false);
  depl.setModStatus(2, false);
  depl.deploy();
  verifyDirsAreEqual(DATA_DIR / "app", DATA_DIR / "source" / "app", true);
}

TEST_CASE("Loadorder is being changed", "[.deployer]")
{
  resetAppDir();
  Deployer depl = Deployer(DATA_DIR / "source", DATA_DIR / "app", "");
  depl.addProfile();
  depl.addMod(2, true);
  depl.addMod(0, true);
  depl.addMod(1, true);
  depl.changeLoadorder(1, 0);
  depl.changeLoadorder(1, 2);
  depl.deploy();
  verifyDirsAreEqual(DATA_DIR / "app", DATA_DIR / "target" / "mod012", true);
}

TEST_CASE("Profiles", "[.deployer]")
{
  resetAppDir();
  Deployer depl = Deployer(DATA_DIR / "source", DATA_DIR / "app", "");
  depl.addProfile();
  depl.addMod(1, true);
  depl.deploy();
  depl.addProfile(0);
  depl.setProfile(1);
  depl.addMod(0, true);
  depl.addMod(2, true);
  depl.changeLoadorder(0, 1);
  depl.deploy();
  SECTION("Copy profile")
  verifyDirsAreEqual(DATA_DIR / "app", DATA_DIR / "target" / "mod012", true);
  SECTION("Create new profile")
  {
    depl.addProfile();
    depl.setProfile(2);
    depl.deploy();
    verifyDirsAreEqual(DATA_DIR / "app", DATA_DIR / "source" / "app", true);
  }
}

TEST_CASE("Get mod conflicts", "[.deployer]")
{
  Deployer depl = Deployer(DATA_DIR / "source", DATA_DIR / "app", "");
  depl.addProfile();
  depl.addMod(0, true);
  depl.addMod(1, true);
  depl.addMod(2, true);
  auto conflicts = depl.getModConflicts(1);
  REQUIRE(conflicts.size() == 1);
  REQUIRE(conflicts.contains(1));
  conflicts = depl.getModConflicts(0);
  REQUIRE(conflicts.size() == 2);
  REQUIRE(conflicts.contains(2));
  REQUIRE(conflicts.contains(0));
}

TEST_CASE("Get file conflicts", "[.deployer]")
{
  Deployer depl = Deployer(DATA_DIR / "source", DATA_DIR / "app", "");
  depl.addProfile();
  depl.addMod(0, true);
  depl.addMod(1, true);
  depl.addMod(2, true);
  auto conflicts = depl.getFileConflicts(1);
  REQUIRE(conflicts.size() == 0);
  conflicts = depl.getFileConflicts(0);
  REQUIRE(conflicts.size() == 3);
}

TEST_CASE("Conflict groups are created", "[.deployer]")
{
  Deployer depl(DATA_DIR / "source" / "conflicts", DATA_DIR / "app", "");
  depl.addProfile();
  for(int i : { 0, 1, 2, 3, 4, 5, 6, 7 })
    depl.addMod(i, true);
  depl.updateConflictGroups();
  auto groups = depl.getConflictGroups();
  REQUIRE_THAT(groups,
               Catch::Matchers::UnorderedEquals(
                 std::vector<std::vector<int>>{ { 0, 1, 2, 3, 5 }, { 4, 6 }, { 7 } }));
}

TEST_CASE("Mods are sorted", "[.deployer]")
{
  Deployer depl(DATA_DIR / "source" / "conflicts", DATA_DIR / "app", "");
  depl.addProfile();
  for(int i : { 5, 6, 0, 7, 4, 2, 1, 3 })
    depl.addMod(i, true);
  depl.sortModsByConflicts();
  REQUIRE_THAT(depl.getLoadorder(),
               Catch::Matchers::UnorderedEquals(std::vector<std::tuple<int, bool>>{ { 5, true },
                                                                                    { 0, true },
                                                                                    { 2, true },
                                                                                    { 1, true },
                                                                                    { 3, true },
                                                                                    { 6, true },
                                                                                    { 4, true },
                                                                                    { 7, true } }));
}

TEST_CASE("Case matching deployer", "[.deployer]")
{
  resetAppDir();
  sfs::remove_all(DATA_DIR / "source" / "case_matching" / "0");
  sfs::remove_all(DATA_DIR / "source" / "case_matching" / "1");
  sfs::copy_options options(sfs::copy_options::recursive | sfs::copy_options::overwrite_existing);
  sfs::copy(DATA_DIR / "source" / "case_matching" / "orig_0",
            DATA_DIR / "source" / "case_matching" / "0",
            options);
  sfs::copy(DATA_DIR / "source" / "case_matching" / "orig_1",
            DATA_DIR / "source" / "case_matching" / "1",
            options);
  CaseMatchingDeployer depl(DATA_DIR / "source" / "case_matching", DATA_DIR / "app", "");
  depl.addProfile();
  depl.addMod(0, true);
  depl.addMod(1, true);
  depl.deploy({ 0, 1 });
  verifyDirsAreEqual(DATA_DIR / "source" / "case_matching" / "0",
                     DATA_DIR / "target" / "case_matching" / "0",
                     false);
  verifyDirsAreEqual(DATA_DIR / "source" / "case_matching" / "1",
                     DATA_DIR / "target" / "case_matching" / "1",
                     false);
}