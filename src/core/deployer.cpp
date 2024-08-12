#include "deployer.h"
#include "pathutils.h"
#include <algorithm>
#include <format>
#include <fstream>
#include <iostream>
#include <json/json.h>
#include <ranges>
#include <set>
#include <unordered_set>

namespace str = std::ranges;
namespace sfs = std::filesystem;
namespace pu = path_utils;


Deployer::Deployer(const sfs::path& source_path,
                   const sfs::path& dest_path,
                   const std::string& name,
                   bool use_copy_deployment) :
  source_path_(source_path), dest_path_(dest_path), name_(name),
  use_copy_deployment_(use_copy_deployment)
{}

std::string Deployer::getDestPath() const
{
  return dest_path_;
}

std::string Deployer::getName() const
{
  return name_;
}

void Deployer::setName(const std::string& name)
{
  name_ = name;
}

std::map<int, unsigned long> Deployer::deploy(const std::vector<int>& loadorder,
                                              std::optional<ProgressNode*> progress_node)
{
  auto [source_files, mod_sizes] = getDeploymentSourceFilesAndModSizes(loadorder);
  log_(Log::LOG_INFO,
       std::format("Deployer '{}': Deploying {} files for {} mods...",
                   name_,
                   source_files.size(),
                   loadorder.size()));
  if(progress_node)
    (*progress_node)->addChildren({ 2, 5, 1 });
  std::map<sfs::path, int> dest_files =
    loadDeployedFiles(progress_node ? &(*progress_node)->child(0) : std::optional<ProgressNode*>{});
  backupOrRestoreFiles(source_files, dest_files);
  deployFiles(source_files,
              progress_node ? &(*progress_node)->child(1) : std::optional<ProgressNode*>{});
  saveDeployedFiles(source_files,
                    progress_node ? &(*progress_node)->child(2) : std::optional<ProgressNode*>{});
  return mod_sizes;
}

std::map<int, unsigned long> Deployer::deploy(std::optional<ProgressNode*> progress_node)
{
  std::vector<int> loadorder;
  for(auto const& [id, enabled] : loadorders_[current_profile_])
  {
    if(enabled)
      loadorder.push_back(id);
  }
  return deploy(loadorder, progress_node);
}

void Deployer::setLoadorder(const std::vector<std::tuple<int, bool>>& loadorder)
{
  loadorders_[current_profile_] = loadorder;
}

std::vector<std::tuple<int, bool>> Deployer::getLoadorder() const
{
  if(loadorders_.empty() || current_profile_ < 0 || current_profile_ >= loadorders_.size() ||
     loadorders_[current_profile_].empty())
    return std::vector<std::tuple<int, bool>>{};
  return loadorders_[current_profile_];
}

std::string Deployer::getType() const
{
  return type_;
}

void Deployer::changeLoadorder(int from_index, int to_index)
{
  if(to_index == from_index)
    return;
  if(to_index < 0 || to_index >= loadorders_[current_profile_].size())
    return;
  if(to_index < from_index)
  {
    std::rotate(loadorders_[current_profile_].begin() + to_index,
                loadorders_[current_profile_].begin() + from_index,
                loadorders_[current_profile_].begin() + from_index + 1);
  }
  else
  {
    std::rotate(loadorders_[current_profile_].begin() + from_index,
                loadorders_[current_profile_].begin() + from_index + 1,
                loadorders_[current_profile_].begin() + to_index + 1);
  }
}

bool Deployer::addMod(int mod_id, bool enabled, bool update_conflicts)
{
  if(hasMod(mod_id))
    return false;
  loadorders_[current_profile_].emplace_back(mod_id, enabled);
  if(update_conflicts && auto_update_conflict_groups_)
    updateConflictGroups();
  return true;
}

bool Deployer::removeMod(int mod_id)
{
  auto iter = std::find_if(loadorders_[current_profile_].begin(),
                           loadorders_[current_profile_].end(),
                           [mod_id](auto elem) { return std::get<0>(elem) == mod_id; });
  if(iter == loadorders_[current_profile_].end())
    return false;
  loadorders_[current_profile_].erase(iter);
  if(auto_update_conflict_groups_)
    updateConflictGroups();
  return true;
}

void Deployer::setModStatus(int mod_id, bool status)
{
  auto iter = std::find_if(loadorders_[current_profile_].begin(),
                           loadorders_[current_profile_].end(),
                           [mod_id, status](const auto& t) { return std::get<0>(t) == mod_id; });
  std::get<1>(*iter) = status;
  return;
}

bool Deployer::hasMod(int mod_id) const
{
  return std::find_if(loadorders_[current_profile_].begin(),
                      loadorders_[current_profile_].end(),
                      [mod_id](const auto& tuple) { return std::get<0>(tuple) == mod_id; }) !=
         loadorders_[current_profile_].end();
}

std::vector<ConflictInfo> Deployer::getFileConflicts(
  int mod_id,
  bool show_disabled,
  std::optional<ProgressNode*> progress_node) const
{
  std::vector<ConflictInfo> conflicts;
  std::unordered_set<std::string> unique_files;
  std::unordered_set<std::string> mod_files = getModFiles(mod_id, false);
  if(!checkModPathExistsAndMaybeLogError(mod_id))
    return conflicts;
  sfs::path mod_base_path = source_path_ / std::to_string(mod_id);
  std::vector<int> loadorder;
  for(auto const& [id, enabled] : loadorders_[current_profile_])
  {
    if(enabled || show_disabled)
      loadorder.push_back(id);
  }

  if(progress_node)
    (*progress_node)->setTotalSteps(loadorder.size());
  bool mod_found = false;
  for(int cur_id : loadorder)
  {
    if(cur_id == mod_id)
    {
      mod_found = true;
      continue;
    }
    if(!checkModPathExistsAndMaybeLogError(cur_id))
      continue;
    mod_base_path = source_path_ / std::to_string(cur_id);
    for(const auto& dir_entry : sfs::recursive_directory_iterator(mod_base_path))
    {
      const auto relative_path = pu::getRelativePath(dir_entry.path(), mod_base_path);
      if(mod_files.contains(relative_path) && !unique_files.contains(relative_path))
      {
        unique_files.insert(relative_path);
        if(mod_found)
          conflicts.emplace_back(relative_path, cur_id, "");
        else
          conflicts.emplace_back(relative_path, mod_id, "");
      }
    }
    if(progress_node)
      (*progress_node)->advance();
  }
  return conflicts;
}

int Deployer::getNumMods() const
{
  return loadorders_[current_profile_].size();
}

const std::filesystem::path& Deployer::destPath() const
{
  return dest_path_;
}

void Deployer::setDestPath(const sfs::path& path)
{
  dest_path_ = path;
}

std::unordered_set<int> Deployer::getModConflicts(int mod_id,
                                                  std::optional<ProgressNode*> progress_node)
{
  std::unordered_set<int> conflicts{ mod_id };
  std::unordered_set<std::string> mod_files = getModFiles(mod_id, false);
  if(!checkModPathExistsAndMaybeLogError(mod_id))
    return conflicts;
  sfs::path mod_base_path = source_path_ / std::to_string(mod_id);
  if(progress_node)
    (*progress_node)->setTotalSteps(loadorders_[current_profile_].size());
  for(const auto [cur_id, _] : loadorders_[current_profile_])
  {
    if(!checkModPathExistsAndMaybeLogError(cur_id))
      continue;
    mod_base_path = source_path_ / std::to_string(cur_id);
    for(const auto& dir_entry : sfs::recursive_directory_iterator(mod_base_path))
    {
      const auto relative_path = pu::getRelativePath(dir_entry.path(), mod_base_path);
      if(mod_files.contains(relative_path))
      {
        conflicts.insert(cur_id);
        break;
      }
    }
    if(progress_node)
      (*progress_node)->advance();
  }
  return conflicts;
}

void Deployer::addProfile(int source)
{
  if(source < 0 || source >= loadorders_.size())
  {
    loadorders_.push_back(std::vector<std::tuple<int, bool>>{});
    conflict_groups_.push_back(std::vector<std::vector<int>>{});
  }
  else
  {
    loadorders_.push_back(loadorders_[source]);
    conflict_groups_.push_back(conflict_groups_[source]);
  }
}

void Deployer::removeProfile(int profile)
{
  loadorders_.erase(loadorders_.begin() + profile);
  conflict_groups_.erase(conflict_groups_.begin() + profile);
  if(profile == current_profile_)
    setProfile(0);
}

void Deployer::setProfile(int profile)
{
  current_profile_ = profile;
}

int Deployer::getProfile() const
{
  return current_profile_;
}

int Deployer::verifyDirectories()
{
  std::string file_name = "_lmm_write_test_file_";
  try
  {
    std::ofstream file(source_path_ / file_name);
    if(file.is_open())
      file << "test";
  }
  catch(const std::ios_base::failure& f)
  {
    return 1;
  }
  try
  {
    if(sfs::exists(dest_path_ / file_name))
      sfs::remove(dest_path_ / file_name);
    if(use_copy_deployment_)
      sfs::copy_file(source_path_ / file_name, dest_path_ / file_name);
    else
      sfs::create_hard_link(source_path_ / file_name, dest_path_ / file_name);
  }
  catch(sfs::filesystem_error& e)
  {
    sfs::remove(source_path_ / file_name);
    if(use_copy_deployment_)
      return 3;
    else
      return 2;
  }
  sfs::remove(source_path_ / file_name);
  sfs::remove(dest_path_ / file_name);
  return 0;
}

bool Deployer::swapMod(int old_id, int new_id)
{
  auto iter = std::find_if(loadorders_[current_profile_].begin(),
                           loadorders_[current_profile_].end(),
                           [old_id](auto elem) { return std::get<0>(elem) == old_id; });
  if(iter == loadorders_[current_profile_].end() || std::get<0>(*iter) == new_id)
    return false;
  *iter = { new_id, std::get<1>(*iter) };
  if(auto_update_conflict_groups_)
    updateConflictGroups();
  return true;
}

void Deployer::sortModsByConflicts(std::optional<ProgressNode*> progress_node)
{
  updateConflictGroups(progress_node);
  std::vector<std::tuple<int, bool>> new_loadorder;
  new_loadorder.reserve(loadorders_[current_profile_].size());
  int i = 0;
  for(const auto& group : conflict_groups_[current_profile_])
  {
    for(int mod_id : group)
    {
      auto entry = str::find_if(loadorders_[current_profile_],
                                [mod_id](auto t) { return std::get<0>(t) == mod_id; });
      new_loadorder.emplace_back(mod_id, std::get<1>(*entry));
    }
    i++;
  }
  loadorders_[current_profile_] = new_loadorder;
}

std::vector<std::vector<int>> Deployer::getConflictGroups() const
{
  return conflict_groups_[current_profile_];
}

void Deployer::setConflictGroups(const std::vector<std::vector<int>>& newConflict_groups)
{
  conflict_groups_[current_profile_] = newConflict_groups;
}

bool Deployer::usesCopyDeployment() const
{
  return use_copy_deployment_;
}

void Deployer::setUseCopyDeployment(bool new_use_copy_deployment)
{
  use_copy_deployment_ = new_use_copy_deployment;
}

bool Deployer::isAutonomous()
{
  return is_autonomous_;
}

std::vector<std::string> Deployer::getModNames() const
{
  return {};
}

std::filesystem::path Deployer::sourcePath() const
{
  return source_path_;
}

void Deployer::setSourcePath(const sfs::path& newSourcePath)
{
  source_path_ = newSourcePath;
}

std::pair<std::map<std::filesystem::path, int>, std::map<int, unsigned long>>
Deployer::getDeploymentSourceFilesAndModSizes(const std::vector<int>& loadorder) const
{
  std::map<sfs::path, int> source_files{};
  std::map<int, unsigned long> mod_sizes{};
  for(int i = loadorder.size() - 1; i >= 0; i--)
  {
    if(!checkModPathExistsAndMaybeLogError(loadorder[i]))
      continue;
    sfs::path mod_base_path = source_path_ / std::to_string(loadorder[i]);
    unsigned long mod_size = 0;
    for(auto const& dir_entry : sfs::recursive_directory_iterator(mod_base_path))
    {
      const bool is_regular_file = dir_entry.is_regular_file();
      if(is_regular_file)
        mod_size += dir_entry.file_size();
      if(is_regular_file || dir_entry.is_directory())
        source_files.insert({ pu::getRelativePath(dir_entry.path(), mod_base_path), loadorder[i] });
    }
    mod_sizes[loadorder[i]] = mod_size;
  }
  return { source_files, mod_sizes };
}

void Deployer::backupOrRestoreFiles(const std::map<sfs::path, int>& source_files,
                                    const std::map<sfs::path, int>& dest_files) const
{
  std::map<sfs::path, int> restore_targets;
  std::map<sfs::path, int> backup_targets;
  std::set_difference(dest_files.begin(),
                      dest_files.end(),
                      source_files.begin(),
                      source_files.end(),
                      std::inserter(restore_targets, restore_targets.begin()),
                      dest_files.value_comp());
  std::set_difference(source_files.begin(),
                      source_files.end(),
                      dest_files.begin(),
                      dest_files.end(),
                      std::inserter(backup_targets, backup_targets.begin()),
                      source_files.value_comp());

  std::map<sfs::path, int> restore_directories;
  for(const auto& [path, id] : restore_targets)
  {
    sfs::path absolute_path = dest_path_ / path;
    if(!sfs::exists(absolute_path))
      continue;
    if(sfs::is_directory(absolute_path))
    {
      restore_directories[path] = id;
      continue;
    }
    sfs::path backup_name = absolute_path.string() + backup_extension_;
    sfs::remove(absolute_path);
    if(sfs::exists(backup_name))
      sfs::rename(backup_name, absolute_path);
  }
  for(const auto& [path, id] : restore_directories)
  {
    sfs::path absolute_path = dest_path_ / path;
    if(pu::directoryIsEmpty(absolute_path))
      sfs::remove_all(absolute_path);
  }

  for(const auto& [path, id] : backup_targets)
  {
    sfs::path absolute_path = dest_path_ / path;
    sfs::path backup_name = absolute_path.string() + backup_extension_;
    if(sfs::exists(absolute_path) && !sfs::is_directory(absolute_path))
      sfs::rename(absolute_path, backup_name);
  }
}

void Deployer::deployFiles(const std::map<sfs::path, int>& source_files,
                           std::optional<ProgressNode*> progress_node) const
{
  if(progress_node)
    (*progress_node)->setTotalSteps(source_files.size());
  for(const auto& [path, id] : source_files)
  {
    sfs::path dest_path = dest_path_ / path;
    if(!checkModPathExistsAndMaybeLogError(id))
      continue;
    sfs::path source_path = source_path_ / std::to_string(id) / path;
    if(sfs::is_directory(source_path) ||
       sfs::exists(dest_path) && sfs::equivalent(source_path, dest_path))
    {
      if(progress_node)
        (*progress_node)->advance();
      continue;
    }
    sfs::create_directories(dest_path.parent_path());
    sfs::remove(dest_path);
    if(use_copy_deployment_)
      sfs::copy_file(source_path, dest_path);
    else
      sfs::create_hard_link(source_path, dest_path);
    if(progress_node)
      (*progress_node)->advance();
  }
}

std::map<sfs::path, int> Deployer::loadDeployedFiles(
  std::optional<ProgressNode*> progress_node) const
{
  if(progress_node)
  {
    (*progress_node)->addChildren({ 1, 2 });
    (*progress_node)->child(0).setTotalSteps(1);
  }
  std::map<sfs::path, int> deployed_files;
  sfs::path deployed_files_path = dest_path_ / deployed_files_name_;
  if(!sfs::exists(deployed_files_path))
    return deployed_files;
  std::ifstream file(deployed_files_path, std::fstream::binary);
  if(!file.is_open())
    throw std::runtime_error("Could not read \"" + deployed_files_path.string() + "\"");
  Json::Value json_object;
  file >> json_object;
  if(progress_node)
  {
    (*progress_node)->child(0).advance();
    (*progress_node)->child(1).setTotalSteps(json_object["files"].size());
  }
  for(int i = 0; i < json_object["files"].size(); i++)
  {
    deployed_files[json_object["files"][i]["path"].asString()] =
      json_object["files"][i]["mod_id"].asInt();
    if(progress_node)
      (*progress_node)->child(1).advance();
  }
  return deployed_files;
}

void Deployer::saveDeployedFiles(const std::map<sfs::path, int>& deployed_files,
                                 std::optional<ProgressNode*> progress_node) const
{
  if(progress_node)
  {
    (*progress_node)->addChildren({ 1, 1 });
    (*progress_node)->child(0).setTotalSteps(deployed_files.size());
    (*progress_node)->child(1).setTotalSteps(1);
  }
  sfs::path deployed_files_path = dest_path_ / deployed_files_name_;
  std::ofstream file(deployed_files_path, std::fstream::binary);
  if(!file.is_open())
    throw std::runtime_error("Could not write \"" + deployed_files_path.string() + "\"");
  Json::Value json_object;
  int i = 0;
  for(auto const& [path, id] : deployed_files)
  {
    json_object["files"][i]["path"] = path.c_str();
    json_object["files"][i]["mod_id"] = id;
    i++;
    if(progress_node)
      (*progress_node)->child(0).advance();
  }
  file << json_object;
  file.close();
  if(progress_node)
    (*progress_node)->child(1).advance();
}

std::unordered_set<std::string> Deployer::getModFiles(int mod_id, bool include_directories) const
{
  std::unordered_set<std::string> mod_files;
  if(!checkModPathExistsAndMaybeLogError(mod_id))
    return mod_files;
  sfs::path mod_base_path = source_path_ / std::to_string(mod_id);
  for(const auto& dir_entry : sfs::recursive_directory_iterator(mod_base_path))
  {
    if(!dir_entry.is_directory() || include_directories)
      mod_files.insert(pu::getRelativePath(dir_entry.path(), mod_base_path));
  }
  return mod_files;
}

bool Deployer::checkModPathExistsAndMaybeLogError(int mod_id) const
{
  if(sfs::exists(source_path_ / std::to_string(mod_id)))
    return true;

  log_(Log::LOG_ERROR, std::format("No installation directory exists for mod with id {}", mod_id));
  return false;
}

void Deployer::updateConflictGroups(std::optional<ProgressNode*> progress_node)
{
  log_(Log::LOG_INFO, std::format("Deployer '{}': Updating conflict groups...", name_));
  std::map<std::string, int> file_map;
  std::vector<std::set<int>> groups;
  std::vector<int> non_conflicting;
  // create groups
  if(progress_node)
    (*progress_node)->setTotalSteps(loadorders_[current_profile_].size());
  for(const auto& [mod_id, _] : loadorders_[current_profile_])
  {
    if(!checkModPathExistsAndMaybeLogError(mod_id))
      continue;
    std::string base_path = (source_path_ / std::to_string(mod_id)).string();
    for(const auto& dir_entry : sfs::recursive_directory_iterator(base_path))
    {
      if(dir_entry.is_directory())
        continue;
      const auto relative_path = pu::getRelativePath(dir_entry.path(), base_path);
      if(!file_map.contains(relative_path))
        file_map[relative_path] = mod_id;
      else
      {
        int other_id = file_map[relative_path];
        auto contains_id = [other_id](const auto& s) { return str::find(s, other_id) != s.end(); };
        auto group_iter = str::find_if(groups, contains_id);
        if(group_iter != groups.end())
          group_iter->insert(mod_id);
        else
          groups.push_back({ other_id, mod_id });
      }
    }
    if(progress_node)
      (*progress_node)->advance();
  }
  std::vector<std::set<int>> merged_groups;
  // merge groups
  for(int i = 0; i < groups.size(); i++)
  {
    if(groups[i].empty())
      continue;
    std::set<int> new_group = groups[i];
    bool found_intersection = true;
    while(found_intersection)
    {
      found_intersection = false;
      for(int j = i + 1; j < groups.size(); j++)
      {
        if(groups[j].empty())
          continue;
        std::vector<int> intersection;
        std::set_intersection(new_group.begin(),
                              new_group.end(),
                              groups[j].begin(),
                              groups[j].end(),
                              std::back_inserter(intersection));
        if(!intersection.empty())
        {
          found_intersection = true;
          new_group.merge(groups[j]);
          groups[j].clear();
        }
      }
    }
    merged_groups.push_back(std::move(new_group));
  }
  std::vector<std::vector<int>> sorted_groups(merged_groups.size() + 1, std::vector<int>());
  // sort mods
  for(const auto& [mod_id, _] : loadorders_[current_profile_])
  {
    bool is_in_group = false;
    for(int i = 0; i < merged_groups.size(); i++)
    {
      if(merged_groups[i].contains(mod_id))
      {
        sorted_groups[i].push_back(mod_id);
        is_in_group = true;
        break;
      }
    }
    if(!is_in_group)
      sorted_groups[sorted_groups.size() - 1].push_back(mod_id);
  }
  conflict_groups_[current_profile_] = sorted_groups;
  log_(Log::LOG_INFO, std::format("Deployer '{}': Conflict groups updated", name_));
}

void Deployer::setLog(const std::function<void(Log::LogLevel, const std::string&)>& newLog)
{
  log_ = newLog;
}

void Deployer::cleanup()
{
  deploy(std::vector<int>{});
  if(sfs::exists(dest_path_ / deployed_files_name_))
    sfs::remove(dest_path_ / deployed_files_name_);
}

bool Deployer::autoUpdateConflictGroups() const
{
  return auto_update_conflict_groups_;
}

void Deployer::setAutoUpdateConflictGroups(bool status)
{
  auto_update_conflict_groups_ = status;
}

std::optional<bool> Deployer::getModStatus(int mod_id)
{
  auto iter = str::find_if(loadorders_[current_profile_],
                           [mod_id](auto t) { return std::get<0>(t) == mod_id; });
  if(iter == loadorders_[current_profile_].end())
    return {};
  return { std::get<1>(*iter) };
}

std::vector<std::vector<std::string>> Deployer::getAutoTags()
{
  return {};
}

std::map<std::string, int> Deployer::getAutoTagMap()
{
  return {};
}