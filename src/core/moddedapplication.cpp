#include "moddedapplication.h"
#include "deployerfactory.h"
#include "installer.h"
#include "parseerror.h"
#include "pathutils.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <ranges>
#include <regex>

namespace sfs = std::filesystem;
namespace str = std::ranges;
namespace pu = path_utils;


ModdedApplication::ModdedApplication(sfs::path staging_dir,
                                     std::string name,
                                     std::string command,
                                     std::filesystem::path icon_path,
                                     std::string app_version) :
  staging_dir_(staging_dir), name_(name), command_(command), icon_path_(icon_path)
{
  if(sfs::exists(staging_dir / CONFIG_FILE_NAME))
    updateState(true);
  else
  {
    addProfile({ "Default", app_version, -1 });
    updateSettings(true);
  }
  sfs::copy(staging_dir_ / CONFIG_FILE_NAME,
            staging_dir_ / ("." + CONFIG_FILE_NAME + ".bak"),
            sfs::copy_options::overwrite_existing);
}

void ModdedApplication::deployMods()
{
  std::vector<int> deployers;
  for(int i = 0; i < deployers_.size(); i++)
    deployers.push_back(i);
  deployModsFor(deployers);
}

void ModdedApplication::deployModsFor(const std::vector<int>& deployers)
{
  std::vector<float> weights;
  for(int i : deployers)
  {
    const int num_mods = deployers_[i]->getNumMods();
    if(deployers_[i]->isAutonomous() || num_mods == 0)
      weights.push_back(1);
    else
      weights.push_back(num_mods);
  }

  // always deploy normal deployers first, since some autonomous deployers
  // may depend on their output
  ProgressNode node(progress_callback_, weights);
  for(int i : deployers)
  {
    if(!deployers_[i]->isAutonomous())
    {
      const auto mod_sizes = deployers_[i]->deploy(&(node.child(i)));
      for(const auto [mod_id, mod_size] : mod_sizes)
      {
        auto mod_iter =
          str::find_if(installed_mods_, [id = mod_id](const Mod& m) { return m.id == id; });
        if(mod_iter != installed_mods_.end())
          mod_iter->size_on_disk = mod_size;
      }
    }
  }

  for(int i : deployers)
  {
    if(deployers_[i]->isAutonomous())
      deployers_[i]->deploy(&(node.child(i)));
  }

  updateSettings(true);
}

void ModdedApplication::installMod(const AddModInfo& info)
{
  if(info.replace_mod && info.group != -1)
  {
    replaceMod(info);
    return;
  }
  ProgressNode progress_node(progress_callback_);
  if(info.group >= 0 && !info.deployers.empty())
    progress_node.addChildren({ 1.0f, 10.0f, info.deployers.size() > 1 ? 10.0f : 1.0f });
  else if(info.group >= 0 || !info.deployers.empty())
    progress_node.addChildren({ 1, 10 });
  else
    progress_node.addChildren({ 1 });
  progress_node.child(0).setTotalSteps(1);
  int mod_id = 0;
  if(!installed_mods_.empty())
    mod_id = std::max_element(installed_mods_.begin(), installed_mods_.end())->id + 1;
  while(sfs::exists(staging_dir_ / std::to_string(mod_id)) &&
        mod_id < std::numeric_limits<int>().max())
    mod_id++;
  if(mod_id == std::numeric_limits<int>().max())
    throw std::runtime_error("Error: Could not generate new mod id.");
  last_mod_id_ = mod_id;
  const auto mod_size = Installer::install(info.source_path,
                                           staging_dir_ / std::to_string(mod_id),
                                           info.installer_flags,
                                           info.installer,
                                           info.root_level,
                                           info.files);
  const auto time_now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  installed_mods_.emplace_back(mod_id,
                               info.name,
                               info.version,
                               time_now,
                               info.local_source,
                               info.remote_source,
                               time_now,
                               mod_size,
                               time_now);
  installer_map_[mod_id] = info.installer;
  progress_node.child(0).advance();
  if(info.group >= 0)
  {
    if(modHasGroup(info.group))
      addModToGroup(mod_id, group_map_[info.group], &progress_node.child(1));
    else
      createGroup(mod_id, info.group, &progress_node.child(1));
  }

  for(int deployer : info.deployers)
    addModToDeployer(deployer, mod_id, true, &progress_node.child(info.group >= 0 ? 2 : 1));

  for(auto& tag : auto_tags_)
    tag.updateMods(staging_dir_, std::vector<int>{ mod_id });
  updateAutoTagMap();

  updateSettings(true);
}

void ModdedApplication::uninstallMods(const std::vector<int>& mod_ids,
                                      const std::string& installer_type)
{
  std::vector<float> weights;
  std::vector<std::vector<int>> update_targets;
  for(int depl = 0; depl < deployers_.size(); depl++)
    update_targets.push_back({});
  for(int mod_id : mod_ids)
  {
    if(group_map_.contains(mod_id))
      removeModFromGroup(mod_id, false);
    auto mod_iter = std::find_if(
      installed_mods_.begin(), installed_mods_.end(), [mod_id](Mod m) { return m.id == mod_id; });
    if(mod_iter == installed_mods_.end())
      continue;
    for(int depl = 0; depl < deployers_.size(); depl++)
    {
      if(deployers_[depl]->isAutonomous())
        continue;
      for(int prof = 0; prof < profile_names_.size(); prof++)
      {
        deployers_[depl]->setProfile(prof);
        if(deployers_[depl]->removeMod(mod_id) &&
           str::find(update_targets[depl], prof) == update_targets[depl].end())
        {
          update_targets[depl].push_back(prof);
          weights.push_back(deployers_[depl]->getNumMods());
        }
      }
      deployers_[depl]->setProfile(current_profile_);
    }

    installed_mods_.erase(mod_iter);
    std::string installer = Installer::SIMPLEINSTALLER;
    if(installer_type == "" && installer_map_.contains(mod_id))
      installer = installer_map_[mod_id];
    Installer::uninstall(staging_dir_ / std::to_string(mod_id), installer);

    for(auto& tag : manual_tags_)
      tag.removeMod(mod_id);
  }

  ProgressNode node(progress_callback_, weights);
  int i = 0;
  for(int depl = 0; depl < update_targets.size(); depl++)
  {
    for(int prof : update_targets[depl])
    {
      deployers_[depl]->setProfile(prof);
      deployers_[depl]->updateConflictGroups(&node.child(i));
      i++;
    }
    deployers_[depl]->setProfile(current_profile_);
  }

  updateSettings(true);
}

void ModdedApplication::changeLoadorder(int deployer, int from_index, int to_index)
{
  deployers_[deployer]->changeLoadorder(from_index, to_index);
  updateSettings(true);
}

void ModdedApplication::addModToDeployer(int deployer,
                                         int mod_id,
                                         bool update_conflicts,
                                         std::optional<ProgressNode*> progress_node)
{
  if(!deployers_[deployer]->isAutonomous())
  {
    const bool was_added = deployers_[deployer]->addMod(mod_id);
    ProgressNode node(progress_callback_);
    if(update_conflicts && was_added)
      deployers_[deployer]->updateConflictGroups(progress_node ? progress_node : &node);
    else if(progress_node)
    {
      (*progress_node)->setTotalSteps(1);
      (*progress_node)->advance();
    }
    splitMod(mod_id, deployer);
    updateSettings(true);
  }
}

void ModdedApplication::removeModFromDeployer(int deployer,
                                              int mod_id,
                                              bool update_conflicts,
                                              std::optional<ProgressNode*> progress_node)
{
  if(!deployers_[deployer]->isAutonomous())
  {
    const bool was_removed = deployers_[deployer]->removeMod(mod_id);
    ProgressNode node(progress_callback_);
    if(update_conflicts && was_removed)
      deployers_[deployer]->updateConflictGroups(progress_node ? progress_node : &node);
    else if(progress_node)
    {
      (*progress_node)->setTotalSteps(1);
      (*progress_node)->advance();
    }
    updateSettings(true);
  }
}

void ModdedApplication::setModStatus(int deployer, int mod_id, bool status)
{
  deployers_[deployer]->setModStatus(mod_id, status);
  updateSettings(true);
}

void ModdedApplication::addDeployer(const EditDeployerInfo& info)
{
  std::string source_dir = staging_dir_;
  if(DeployerFactory::AUTONOMOUS_DEPLOYERS.at(info.type))
    source_dir = info.source_dir;
  deployers_.push_back(DeployerFactory::makeDeployer(
    info.type, source_dir, info.target_dir, info.name, info.use_copy_deployment));
  for(int i = 0; i < profile_names_.size(); i++)
    deployers_[deployers_.size() - 1]->addProfile();
  deployers_[deployers_.size() - 1]->setProfile(current_profile_);
  for(int i = 0; i < installed_mods_.size(); i++)
  {
    for(int depl = 0; depl < deployers_.size(); depl++)
    {
      if(deployers_[depl]->hasMod(installed_mods_[i].id))
        splitMod(installed_mods_[i].id, depl);
    }
  }
  updateSettings(true);
}

void ModdedApplication::removeDeployer(int deployer, bool cleanup)
{
  if(cleanup)
    deployers_[deployer]->cleanup();
  deployers_.erase(deployers_.begin() + deployer);
  updateSettings(true);
}

std::vector<std::string> ModdedApplication::getDeployerNames() const
{
  std::vector<std::string> names;
  for(const auto& deployer : deployers_)
    names.push_back(deployer->getName());
  return names;
}

std::vector<ModInfo> ModdedApplication::getModInfo() const
{
  std::vector<ModInfo> mod_info{};
  for(const auto& mod : installed_mods_)
  {
    std::vector<std::string> deployer_names;
    std::vector<int> deployer_ids;
    std::vector<bool> statuses;
    for(int i = 0; i < deployers_.size(); i++)
    {
      if(deployers_[i]->isAutonomous())
        continue;
      auto status = deployers_[i]->getModStatus(mod.id);
      if(status)
      {
        deployer_names.push_back(deployers_[i]->getName());
        deployer_ids.push_back(i);
        statuses.push_back(*status);
      }
    }

    int group = -1;
    bool is_active = false;
    if(group_map_.contains(mod.id))
    {
      group = group_map_.at(mod.id);
      is_active = active_group_members_[group] == mod.id;
    }

    mod_info.emplace_back(
      mod.id,
      mod.name,
      mod.version,
      mod.install_time,
      mod.local_source,
      mod.remote_source,
      mod.remote_update_time,
      mod.size_on_disk,
      mod.suppress_update_time,
      deployer_names,
      deployer_ids,
      statuses,
      group,
      is_active,
      manual_tag_map_.contains(mod.id) ? manual_tag_map_.at(mod.id) : std::vector<std::string>{},
      auto_tag_map_.contains(mod.id) ? auto_tag_map_.at(mod.id) : std::vector<std::string>{});
  }
  return mod_info;
}

std::vector<std::tuple<int, bool>> ModdedApplication::getLoadorder(int deployer) const
{
  return deployers_[deployer]->getLoadorder();
}

const sfs::path& ModdedApplication::getStagingDir() const
{
  return staging_dir_;
}

void ModdedApplication::setStagingDir(std::string staging_dir, bool move_existing)
{
  if(staging_dir == staging_dir_)
    return;
  if(move_existing)
  {
    for(const auto& mod : installed_mods_)
    {
      std::string mod_dir = std::to_string(mod.id);
      sfs::rename(staging_dir_ / mod_dir, sfs::path(staging_dir) / mod_dir);
    }
    sfs::rename(staging_dir_ / CONFIG_FILE_NAME, sfs::path(staging_dir) / CONFIG_FILE_NAME);
  }
  staging_dir_ = staging_dir;
  updateState(true);
}

const std::string& ModdedApplication::name() const
{
  return name_;
}

void ModdedApplication::setName(const std::string& newName)
{
  name_ = newName;
  updateSettings(true);
}

int ModdedApplication::getNumDeployers() const
{
  return deployers_.size();
}

const std::string& ModdedApplication::getConfigFileName() const
{
  return CONFIG_FILE_NAME;
}

void ModdedApplication::changeModName(int mod_id, const std::string& new_name)
{
  auto iter = std::find_if(
    installed_mods_.begin(), installed_mods_.end(), [mod_id](Mod m) { return m.id == mod_id; });
  if(iter == installed_mods_.end())
    throw std::runtime_error("Error: Unknown mod id: " + std::to_string(mod_id));
  iter->name = new_name;
  updateSettings(true);
}

std::vector<ConflictInfo> ModdedApplication::getFileConflicts(int deployer,
                                                              int mod_id,
                                                              bool show_disabled) const
{
  ProgressNode node(progress_callback_);
  auto conflicts = deployers_[deployer]->getFileConflicts(mod_id, show_disabled, &node);
  for(auto& [_, id, name] : conflicts)
    name = getModName(id);
  return conflicts;
}

AppInfo ModdedApplication::getAppInfo() const
{
  AppInfo info;
  info.name = name_;
  info.staging_dir = staging_dir_.string();
  info.command = command_;
  info.num_mods = installed_mods_.size();
  info.app_version = app_versions_[current_profile_];
  for(const auto& deployer : deployers_)
  {
    info.deployers.push_back(deployer->getName());
    info.deployer_types.push_back(deployer->getType());
    info.target_dirs.push_back(deployer->getDestPath());
    info.deployer_mods.push_back(deployer->getNumMods());
    info.uses_copy_deployment.push_back(deployer->usesCopyDeployment());
  }
  info.tools = tools_;
  for(const auto& tag : manual_tags_)
    info.num_mods_per_manual_tag[tag.getName()] = tag.getNumMods();
  for(const auto& tag : auto_tags_)
  {
    info.num_mods_per_auto_tag[tag.getName()] = tag.getNumMods();
    info.auto_tags[tag.getName()] = { tag.getExpression(), tag.getConditions() };
  }
  return info;
}

void ModdedApplication::addTool(std::string name, std::string command)
{
  tools_.emplace_back(name, command);
  updateSettings(true);
}

void ModdedApplication::removeTool(int tool_id)
{
  if(tool_id < tools_.size() && tool_id >= 0)
  {
    tools_.erase(tools_.begin() + tool_id);
    updateSettings(true);
  }
}

const std::vector<std::tuple<std::string, std::string>>& ModdedApplication::getTools() const
{
  return tools_;
}

const std::string& ModdedApplication::command() const
{
  return command_;
}

void ModdedApplication::setCommand(const std::string& newCommand)
{
  command_ = newCommand;
  updateSettings(true);
}

void ModdedApplication::editDeployer(int deployer, const EditDeployerInfo& info)
{
  if(deployers_[deployer]->getType() == info.type)
  {
    deployers_[deployer]->setName(info.name);
    deployers_[deployer]->setDestPath(info.target_dir);
    deployers_[deployer]->setUseCopyDeployment(info.use_copy_deployment);
  }
  else
  {
    json_settings_["deployers"][deployer]["source_path"] = info.source_dir;
    json_settings_["deployers"][deployer]["name"] = info.name;
    json_settings_["deployers"][deployer]["dest_path"] = info.target_dir;
    json_settings_["deployers"][deployer]["type"] = info.type;
    json_settings_["deployers"][deployer]["use_copy_deployment"] = info.use_copy_deployment;
    updateState();
  }
  if(deployers_[deployer]->isAutonomous())
    deployers_[deployer]->setSourcePath(info.source_dir);
  updateSettings(true);
}

std::unordered_set<int> ModdedApplication::getModConflicts(int deployer, int mod_id)
{
  ProgressNode node(progress_callback_);
  return deployers_[deployer]->getModConflicts(mod_id, &node);
}

void ModdedApplication::setProfile(int profile)
{
  if(profile < 0 || profile >= profile_names_.size())
    return;
  bak_man_.setProfile(profile);
  for(const auto& deployer : deployers_)
    deployer->setProfile(profile);
  current_profile_ = profile;
}

void ModdedApplication::addProfile(const EditProfileInfo& info)
{
  profile_names_.push_back(info.name);
  app_versions_.push_back(info.app_version);
  for(const auto& deployer : deployers_)
    deployer->addProfile(info.source);
  bak_man_.addProfile(info.source);
  updateSettings(true);
}

void ModdedApplication::removeProfile(int profile)
{
  if(profile < 0 || profile >= profile_names_.size())
    return;
  for(const auto& deployer : deployers_)
    deployer->removeProfile(profile);
  profile_names_.erase(profile_names_.begin() + profile);
  app_versions_.erase(app_versions_.begin() + profile);
  bak_man_.removeProfile(profile);
  if(profile == current_profile_)
    setProfile(0);
  updateSettings(true);
}

std::vector<std::string> ModdedApplication::getProfileNames() const
{
  return profile_names_;
}

void ModdedApplication::editProfile(int profile, const EditProfileInfo& info)
{
  if(profile < 0 || profile >= profile_names_.size())
    return;
  profile_names_[profile] = info.name;
  app_versions_[profile] = info.app_version;
  updateSettings(true);
}

void ModdedApplication::editTool(int tool, std::string name, std::string command)
{
  if(tool >= 0 && tool < tools_.size())
  {
    std::get<0>(tools_[tool]) = name;
    std::get<1>(tools_[tool]) = command;
  }
  updateSettings(true);
}

std::tuple<int, std::string> ModdedApplication::verifyDeployerDirectories()
{
  std::tuple<int, std::string> ret{ 0, "" };
  for(const auto& depl : deployers_)
  {
    int cur_code = depl->verifyDirectories();
    if(cur_code)
    {
      ret = std::tuple<int, std::string>{ cur_code, depl->destPath() };
    }
  }
  return ret;
}

void ModdedApplication::addModToGroup(int mod_id,
                                      int group,
                                      std::optional<ProgressNode*> progress_node)
{
  if(group < 0 || group >= groups_.size() || group_map_.contains(mod_id))
    return;
  groups_[group].push_back(mod_id);
  group_map_[mod_id] = group;
  active_group_members_[group] = mod_id;
  ProgressNode node(progress_callback_);
  updateDeployerGroups(progress_node ? progress_node : &node);
  updateSettings(true);
}

void ModdedApplication::removeModFromGroup(int mod_id,
                                           bool update_conflicts,
                                           std::optional<ProgressNode*> progress_node)
{
  if(!group_map_.contains(mod_id))
    return;
  int group = group_map_[mod_id];
  groups_[group].erase(std::find(groups_[group].begin(), groups_[group].end(), mod_id));

  if(!groups_[group].empty())
  {
    active_group_members_[group] = groups_[group][0];
    std::vector<std::vector<int>> update_targets;
    std::vector<float> weights;
    for(int depl = 0; depl < deployers_.size(); depl++)
    {
      update_targets.push_back({});
      if(deployers_[depl]->isAutonomous())
        continue;
      for(int prof = 0; prof < profile_names_.size(); prof++)
      {
        deployers_[depl]->setProfile(prof);
        auto loadorder = deployers_[depl]->getLoadorder();
        auto iter = str::find_if(
          loadorder, [mod_id](const auto& tuple) { return std::get<0>(tuple) == mod_id; });
        if(iter != loadorder.end())
        {
          deployers_[depl]->addMod(active_group_members_[group], std::get<1>(*iter), false);
          deployers_[depl]->changeLoadorder(loadorder.size(), iter - loadorder.begin());
          update_targets[depl].push_back(prof);
          weights.push_back(loadorder.size());
        }
      }
      deployers_[depl]->setProfile(current_profile_);
    }

    ProgressNode node = progress_node ? **progress_node : ProgressNode(progress_callback_);
    if(!update_conflicts)
    {
      node.setTotalSteps(1);
      node.advance();
    }
    else
    {
      node.addChildren(weights);
      int i = 0;
      for(int depl = 0; depl < update_targets.size(); depl++)
      {
        for(int prof : update_targets[depl])
        {
          deployers_[depl]->setProfile(prof);
          deployers_[depl]->updateConflictGroups(&node.child(i));
          i++;
        }
        deployers_[depl]->setProfile(current_profile_);
      }
    }
  }

  if(groups_[group].size() == 1)
    group_map_.erase(groups_[group][0]);
  if(groups_[group].size() < 2)
  {
    groups_.erase(groups_.begin() + group);
    active_group_members_.erase(active_group_members_.begin() + group);
    for(auto& pair : group_map_)
    {
      if(pair.second > group)
        pair.second--;
    }
  }
  group_map_.erase(mod_id);
  updateSettings(true);
}

void ModdedApplication::createGroup(int first_mod_id,
                                    int second_mod_id,
                                    std::optional<ProgressNode*> progress_node)
{
  if(group_map_.contains(first_mod_id))
  {
    addModToGroup(second_mod_id, group_map_[first_mod_id]);
    return;
  }
  if(group_map_.contains(second_mod_id))
  {
    addModToGroup(first_mod_id, group_map_[second_mod_id]);
    return;
  }
  groups_.push_back({ first_mod_id, second_mod_id });
  int group = groups_.size() - 1;
  group_map_[first_mod_id] = group;
  group_map_[second_mod_id] = group;
  active_group_members_.push_back(first_mod_id);
  ProgressNode node(progress_callback_);
  updateDeployerGroups(progress_node ? progress_node : &node);
  updateSettings(true);
}

void ModdedApplication::changeActiveGroupMember(int group,
                                                int mod_id,
                                                std::optional<ProgressNode*> progress_node)
{
  if(group < 0 || group >= groups_.size() ||
     std::find(groups_[group].begin(), groups_[group].end(), mod_id) == groups_[group].end())
    return;
  active_group_members_[group] = mod_id;
  ProgressNode node(progress_callback_);
  updateDeployerGroups(progress_node ? progress_node : &node);
  updateSettings(true);
}

void ModdedApplication::changeModVersion(int mod_id, const std::string& new_version)
{
  auto iter = std::find_if(
    installed_mods_.begin(), installed_mods_.end(), [mod_id](Mod m) { return m.id == mod_id; });
  if(iter == installed_mods_.end())
    throw std::runtime_error("Error: Unknown mod id: " + std::to_string(mod_id));
  iter->version = new_version;
  updateSettings(true);
}

int ModdedApplication::getNumGroups()
{
  return groups_.size();
}

bool ModdedApplication::modHasGroup(int mod_id)
{
  return group_map_.contains(mod_id);
}

int ModdedApplication::getModGroup(int mod_id)
{
  if(!group_map_.contains(mod_id))
    return -1;
  return group_map_[mod_id];
}

void ModdedApplication::sortModsByConflicts(int deployer)
{
  ProgressNode node(progress_callback_);
  deployers_[deployer]->sortModsByConflicts(&node);
  updateSettings(true);
}

std::vector<std::vector<int>> ModdedApplication::getConflictGroups(int deployer)
{
  return deployers_[deployer]->getConflictGroups();
}

void ModdedApplication::updateModDeployers(const std::vector<int>& mod_ids,
                                           const std::vector<bool>& deployers)
{
  std::vector<float> weights;
  for(const auto& depl : deployers_)
    weights.push_back(depl->isAutonomous() ? 1 : depl->getNumMods());
  ProgressNode node(progress_callback_, weights);
  std::optional<ProgressNode*> dummy_node{};
  for(int i = 0; i < mod_ids.size(); i++)
  {
    const int mod_id = mod_ids[i];
    const bool is_last_mod = i == (mod_ids.size() - 1);
    for(int depl = 0; depl < deployers.size(); depl++)
    {
      if(deployers_[depl]->isAutonomous())
        continue;
      if(deployers[depl])
        addModToDeployer(depl, mod_id, is_last_mod, is_last_mod ? &node.child(depl) : dummy_node);
      else
        removeModFromDeployer(
          depl, mod_id, is_last_mod, is_last_mod ? &node.child(depl) : dummy_node);
    }
  }
}

int ModdedApplication::verifyStagingDir(sfs::path staging_dir)
{
  try
  {
    Json::Value val;
    std::ifstream file(staging_dir / CONFIG_FILE_NAME, std::fstream::binary);
    if(file.is_open())
      file >> val;
    file.close();
  }
  catch(std::ios_base::failure& f)
  {
    return 1;
  }
  catch(Json::RuntimeError& e)
  {
    return 2;
  }
  return 0;
}

void ModdedApplication::extractArchive(const sfs::path& source, const sfs::path& target)
{
  ProgressNode node(progress_callback_);
  Installer::extract(source, target, &node);
}

DeployerInfo ModdedApplication::getDeployerInfo(int deployer)
{
  if(!(deployers_[deployer]->isAutonomous()))
  {
    std::map<std::string, int> mods_per_tag;
    for(const auto& tag : manual_tags_)
      mods_per_tag[tag.getName()] = tag.getNumMods();

    const auto loadorder = deployers_[deployer]->getLoadorder();
    std::vector<std::string> mod_names;
    mod_names.reserve(loadorder.size());
    std::vector<std::vector<std::string>> manual_tags;
    manual_tags.reserve(loadorder.size());
    std::vector<std::vector<std::string>> auto_tags;
    manual_tags.reserve(loadorder.size());
    for(const auto& [id, e] : loadorder)
    {
      mod_names.push_back(
        std::ranges::find_if(installed_mods_, [id = id](auto& mod) { return mod.id == id; })->name);
      if(manual_tag_map_.contains(id))
        manual_tags.push_back(manual_tag_map_.at(id));
      else
        manual_tags.push_back({});

      if(auto_tag_map_.contains(id))
        auto_tags.push_back(auto_tag_map_.at(id));
      else
        auto_tags.push_back({});
    }
    for(const auto& tag : auto_tags_)
    {
      if(mods_per_tag.contains(tag.getName()))
        mods_per_tag[tag.getName()] += tag.getNumMods();
      else
        mods_per_tag[tag.getName()] = tag.getNumMods();
    }
    return { mod_names, loadorder,   deployers_[deployer]->getConflictGroups(), false, manual_tags,
             auto_tags, mods_per_tag };
  }
  else
  {
    return { deployers_[deployer]->getModNames(),
             deployers_[deployer]->getLoadorder(),
             deployers_[deployer]->getConflictGroups(),
             true,
             {},
             deployers_[deployer]->getAutoTags(),
             deployers_[deployer]->getAutoTagMap() };
  }
}

void ModdedApplication::setLog(const std::function<void(Log::LogLevel, const std::string&)>& newLog)
{
  log_ = newLog;
  for(auto& deployer : deployers_)
    deployer->setLog(newLog);
}

void ModdedApplication::addBackupTarget(const sfs::path& path,
                                        const std::string& name,
                                        const std::vector<std::string>& backup_names)
{
  bak_man_.addTarget(path, name, backup_names);
  updateSettings(true);
}

void ModdedApplication::removeBackupTarget(int target_id)
{
  if(target_id < 0 || target_id >= bak_man_.getNumTargets())
    return;
  bak_man_.removeTarget(target_id);
  updateSettings(true);
}

void ModdedApplication::removeAllBackupTargets()
{
  for(int target = 0; target < bak_man_.getNumTargets(); target++)
    removeBackupTarget(target);
}

void ModdedApplication::addBackup(int target_id, const std::string& name, int source)
{
  if(target_id < 0 || target_id >= bak_man_.getNumTargets())
    return;
  bak_man_.addBackup(target_id, name, source);
}

void ModdedApplication::removeBackup(int target_id, int backup_id)
{
  if(target_id < 0 || target_id >= bak_man_.getNumTargets() || backup_id < 0 ||
     backup_id >= bak_man_.getNumBackups(target_id))
    return;
  bak_man_.removeBackup(target_id, backup_id);
}

void ModdedApplication::setActiveBackup(int target_id, int backup_id)
{
  if(target_id < 0 || target_id >= bak_man_.getNumTargets() || backup_id < 0 ||
     backup_id >= bak_man_.getNumBackups(target_id))
    return;
  bak_man_.setActiveBackup(target_id, backup_id);
}

std::vector<BackupTarget> ModdedApplication::getBackupTargets() const
{
  return bak_man_.getTargets();
}

void ModdedApplication::setBackupName(int target_id, int backup_id, const std::string& name)
{
  if(target_id < 0 || target_id >= bak_man_.getNumTargets() || backup_id < 0 ||
     backup_id >= bak_man_.getNumBackups(target_id))
    return;
  bak_man_.setBackupName(target_id, backup_id, name);
}

void ModdedApplication::setBackupTargetName(int target_id, const std::string& name)
{
  if(target_id < 0 || target_id >= bak_man_.getNumTargets())
    return;
  bak_man_.setBackupTargetName(target_id, name);
}

void ModdedApplication::overwriteBackup(int target_id, int source_backup, int dest_backup)
{
  if(target_id < 0 || target_id >= bak_man_.getNumTargets())
    return;
  bak_man_.overwriteBackup(target_id, source_backup, dest_backup);
}

void ModdedApplication::cleanupFailedInstallation()
{
  Installer::cleanupFailedInstallation(staging_dir_, last_mod_id_);
  auto iter = std::find_if(installed_mods_.begin(),
                           installed_mods_.end(),
                           [this](const Mod& m) { return m.id == this->last_mod_id_; });
  if(iter != installed_mods_.end())
    uninstallMods({ last_mod_id_ });
  last_mod_id_ = -1;
}

void ModdedApplication::setProgressCallback(const std::function<void(float)>& progress_callback)
{
  progress_callback_ = progress_callback;
}

void ModdedApplication::uninstallGroupMembers(const std::vector<int>& mod_ids)
{
  std::vector<int> uninstall_targets;
  for(int active_id : mod_ids)
  {
    if(!group_map_.contains(active_id))
      continue;
    for(int mod_id : groups_[group_map_[active_id]])
    {
      if(mod_id != active_id)
        uninstall_targets.push_back(mod_id);
    }
  }
  uninstallMods(uninstall_targets);
}

void ModdedApplication::addManualTag(const std::string& tag_name)
{
  if(str::find(manual_tags_, tag_name) != manual_tags_.end())
    throw std::runtime_error(
      std::format("Error: A tag with the name '{}' already exists.", tag_name));
  manual_tags_.emplace_back(tag_name);
  updateSettings(true);
}

void ModdedApplication::removeManualTag(const std::string& tag_name, bool update_map)
{
  auto iter = str::find(manual_tags_, tag_name);
  if(iter != manual_tags_.end())
    manual_tags_.erase(iter);
  if(update_map)
    updateManualTagMap();
  updateSettings(true);
}

void ModdedApplication::changeManualTagName(const std::string& old_name,
                                            const std::string& new_name,
                                            bool update_map)
{
  auto old_iter = str::find(manual_tags_, old_name);
  if(old_iter == manual_tags_.end())
    return;
  auto new_iter = str::find(manual_tags_, new_name);
  if(new_iter != manual_tags_.end())
    throw std::runtime_error(
      std::format("Error: Cannot rename tag '{}', because a tag with the name '{}' already exists.",
                  old_name,
                  new_name));
  old_iter->setName(new_name);
  if(update_map)
    updateManualTagMap();
  updateSettings(true);
}

void ModdedApplication::addTagsToMods(const std::vector<std::string>& tag_names,
                                      const std::vector<int>& mod_ids)
{
  for(const auto& tag_name : tag_names)
  {
    auto tag = str::find(manual_tags_, tag_name);
    if(tag == manual_tags_.end())
      return;
    for(int mod : mod_ids)
      tag->addMod(mod);
  }
  updateManualTagMap();
  updateSettings(true);
}

void ModdedApplication::removeTagsFromMods(const std::vector<std::string>& tag_names,
                                           const std::vector<int>& mod_ids)
{
  for(const auto& tag_name : tag_names)
  {
    auto tag = str::find(manual_tags_, tag_name);
    if(tag == manual_tags_.end())
      return;
    for(int mod : mod_ids)
      tag->removeMod(mod);
  }
  updateManualTagMap();
  updateSettings(true);
}

void ModdedApplication::setTagsForMods(const std::vector<std::string>& tag_names,
                                       const std::vector<int> mod_ids)
{
  for(auto& tag : manual_tags_)
  {
    if(str::find(tag_names, tag) != tag_names.end())
    {
      for(int mod : mod_ids)
        tag.addMod(mod);
    }
    else
    {
      for(int mod : mod_ids)
        tag.removeMod(mod);
    }
  }
  updateManualTagMap();
  updateSettings(true);
}

void ModdedApplication::editManualTags(const std::vector<EditManualTagAction>& actions)
{
  auto old_tags = manual_tags_;
  try
  {
    for(const auto& action : actions)
    {
      if(action.getType() == EditManualTagAction::ActionType::add)
        addManualTag(action.getName());
      else if(action.getType() == EditManualTagAction::ActionType::remove)
        removeManualTag(action.getName(), false);
      else if(action.getType() == EditManualTagAction::ActionType::rename)
        changeManualTagName(action.getName(), action.getNewName(), false);
    }
  }
  catch(std::runtime_error& e)
  {
    manual_tags_ = old_tags;
    throw e;
  }
  updateManualTagMap();
  updateSettings(true);
}

void ModdedApplication::addAutoTag(const std::string& tag_name,
                                   const std::string& expression,
                                   const std::vector<TagCondition>& conditions,
                                   bool update)
{
  if(std::find(auto_tags_.begin(), auto_tags_.end(), tag_name) != auto_tags_.end())
    throw std::runtime_error(
      std::format("Error: A tag with the name '{}' already exists.", tag_name));

  auto_tags_.emplace_back(tag_name, expression, conditions);
  auto select_id = [](const auto& mod) { return mod.id; };
  if(expression != "")
    auto_tags_.back().reapplyMods(staging_dir_, str::transform_view(installed_mods_, select_id));
  if(update)
  {
    updateAutoTagMap();
    updateSettings(true);
  }
}

void ModdedApplication::removeAutoTag(const std::string& tag_name, bool update)
{
  auto iter = std::find(auto_tags_.begin(), auto_tags_.end(), tag_name);
  if(iter == auto_tags_.end())
    return;
  auto_tags_.erase(iter);
  if(update)
  {
    updateAutoTagMap();
    updateSettings(true);
  }
}

void ModdedApplication::renameAutoTag(const std::string& old_name,
                                      const std::string& new_name,
                                      bool update)
{
  auto iter = std::find(auto_tags_.begin(), auto_tags_.end(), old_name);
  if(iter == auto_tags_.end())
    return;
  if(std::find(auto_tags_.begin(), auto_tags_.end(), new_name) != auto_tags_.end())
    throw std::runtime_error(
      std::format("Error: Cannot rename tag '{}', because a tag with the name '{}' already exists.",
                  old_name,
                  new_name));

  iter->setName(new_name);
  if(update)
  {
    updateAutoTagMap();
    updateSettings(true);
  }
}

void ModdedApplication::changeAutoTagEvaluator(const std::string& tag_name,
                                               const std::string& expression,
                                               const std::vector<TagCondition>& conditions,
                                               bool update)
{
  auto iter = std::find(auto_tags_.begin(), auto_tags_.end(), tag_name);
  if(iter == auto_tags_.end())
    return;

  iter->setEvaluator(expression, conditions);
  auto select_id = [](const auto& mod) { return mod.id; };
  if(update)
  {
    iter->reapplyMods(staging_dir_, str::transform_view(installed_mods_, select_id));
    updateAutoTagMap();
    updateSettings(true);
  }
}

void ModdedApplication::editAutoTags(const std::vector<EditAutoTagAction>& actions)
{
  auto old_tags = auto_tags_;
  try
  {
    std::vector<std::string> reapply_targets;
    for(const auto& action : actions)
    {
      if(action.getType() == EditAutoTagAction::ActionType::add)
        addAutoTag(action.getName(), action.getExpression(), action.getConditions(), false);
      else if(action.getType() == EditAutoTagAction::ActionType::remove)
        removeAutoTag(action.getName(), false);
      else if(action.getType() == EditAutoTagAction::ActionType::rename)
        renameAutoTag(action.getName(), action.getNewName(), false);
      else if(action.getType() == EditAutoTagAction::ActionType::change_evaluator)
      {
        changeAutoTagEvaluator(
          action.getName(), action.getExpression(), action.getConditions(), false);
        reapply_targets.push_back(action.getName());
      }
    }
    if(!reapply_targets.empty())
    {
      log_(Log::LOG_INFO, "Reapplying auto tags with edited conditions to all mods...");
      ProgressNode node(progress_callback_);
      node.addChildren({ 1.0f, std::min(8.0f, (float)reapply_targets.size()) });
      node.child(0).setTotalSteps(installed_mods_.size());
      std::vector<float> weights;
      for(const auto& tag : reapply_targets)
      {
        auto iter = std::find(auto_tags_.begin(), auto_tags_.end(), tag);
        if(iter != auto_tags_.end())
          weights.push_back(iter->getNumConditions());
      }
      node.child(1).addChildren(weights);
      for(int i = 0; i < weights.size(); i++)
        node.child(1).child(i).setTotalSteps(installed_mods_.size());

      auto select_id = [](const auto& mod) { return mod.id; };
      auto mods = str::transform_view(installed_mods_, select_id);
      const auto files = AutoTag::readModFiles(staging_dir_, mods, &node.child(0));
      for(int i = 0; i < reapply_targets.size(); i++)
      {
        auto iter = std::find(auto_tags_.begin(), auto_tags_.end(), reapply_targets[i]);
        if(iter != auto_tags_.end())
          iter->reapplyMods(files, mods, &node.child(1).child(i));
      }
    }
  }
  catch(std::runtime_error& e)
  {
    auto_tags_ = old_tags;
    throw e;
  }
  updateAutoTagMap();
  updateSettings(true);
}

void ModdedApplication::reapplyAutoTags()
{
  log_(Log::LOG_INFO, "Reapplying auto tags to all mods...");
  ProgressNode node(progress_callback_);
  node.addChildren({ 1.0f, 8.0f });
  node.child(0).setTotalSteps(installed_mods_.size());
  std::vector<float> weights;
  for(auto& tag : auto_tags_)
    weights.push_back(tag.getNumConditions());
  node.child(1).addChildren(weights);
  for(int i = 0; i < weights.size(); i++)
    node.child(1).child(i).setTotalSteps(installed_mods_.size());
  auto select_id = [](const auto& mod) { return mod.id; };
  auto mods = str::transform_view(installed_mods_, select_id);
  const auto files = AutoTag::readModFiles(staging_dir_, mods, &node.child(0));
  for(int i = 0; i < auto_tags_.size(); i++)
    auto_tags_[i].reapplyMods(files, mods, &node.child(1).child(i));
  updateAutoTagMap();
  updateSettings(true);
}

void ModdedApplication::updateAutoTags(const std::vector<int> mod_ids)
{
  log_(Log::LOG_INFO, std::format("Reapplying auto tags to {} mods...", mod_ids.size()));
  ProgressNode node(progress_callback_);
  node.addChildren(
    { 1.0f, std::max(1.0f, 8.0f * (float)mod_ids.size() / (float)installed_mods_.size()) });
  node.child(0).setTotalSteps(mod_ids.size());
  std::vector<float> weights;
  for(auto& tag : auto_tags_)
    weights.push_back(tag.getNumConditions());
  node.child(1).addChildren(weights);
  for(int i = 0; i < weights.size(); i++)
    node.child(1).child(i).setTotalSteps(mod_ids.size());
  const auto files = AutoTag::readModFiles(staging_dir_, mod_ids, &node.child(0));
  for(int i = 0; i < auto_tags_.size(); i++)
    auto_tags_[i].updateMods(files, mod_ids, &node.child(1).child(i));
  updateAutoTagMap();
  updateSettings(true);
}

void ModdedApplication::deleteAllData()
{
  for(int i = 0; i < deployers_.size(); i++)
    removeDeployer(i, true);
  for(auto mod : installed_mods_)
  {
    const auto path = staging_dir_ / std::to_string(mod.id);
    if(sfs::exists(path))
      sfs::remove_all(path);
  }
  const auto path = staging_dir_ / CONFIG_FILE_NAME;
  if(sfs::exists(path))
    sfs::remove(path);

  if(sfs::exists(staging_dir_ / download_dir_))
    sfs::remove_all(staging_dir_ / download_dir_);
}

void ModdedApplication::setAppVersion(const std::string& app_version)
{
  app_versions_[current_profile_] = app_version;
  updateSettings(true);
}

void ModdedApplication::setModSources(int mod_id,
                                      const std::string& local_source,
                                      const std::string& remote_source)
{
  auto iter = std::find_if(
    installed_mods_.begin(), installed_mods_.end(), [mod_id](Mod m) { return m.id == mod_id; });
  if(iter == installed_mods_.end())
    throw std::runtime_error("Error: Unknown mod id: " + std::to_string(mod_id));
  iter->local_source = local_source;
  iter->remote_source = remote_source;
  updateSettings(true);
}

nexus::Page ModdedApplication::getNexusPage(int mod_id)
{
  auto iter = std::find_if(
    installed_mods_.begin(), installed_mods_.end(), [mod_id](Mod m) { return m.id == mod_id; });
  if(iter == installed_mods_.end())
    throw std::runtime_error("Error: Unknown mod id: " + std::to_string(mod_id));
  return nexus::Api::getNexusPage(iter->remote_source);
}

void ModdedApplication::checkForModUpdates()
{
  std::vector<int> target_mod_indices;
  for(const auto& [i, mod] : str::enumerate_view(installed_mods_))
  {
    if(nexus::Api::modUrlIsValid(mod.remote_source) && mod.remote_update_time <= mod.install_time)
      target_mod_indices.push_back(i);
  }
  performUpdateCheck(target_mod_indices);
}

void ModdedApplication::checkModsForUpdates(const std::vector<int>& mod_ids)
{
  std::vector<int> target_mod_indices;
  for(const auto& [i, mod] : str::enumerate_view(installed_mods_))
  {
    if(str::find(mod_ids, mod.id) != mod_ids.end() &&
       nexus::Api::modUrlIsValid(mod.remote_source) && mod.remote_update_time <= mod.install_time)
      target_mod_indices.push_back(i);
  }
  performUpdateCheck(target_mod_indices);
}

void ModdedApplication::suppressUpdateNotification(const std::vector<int>& mod_ids)
{
  for(int mod_id : mod_ids)
  {
    auto iter = std::find_if(installed_mods_.begin(),
                             installed_mods_.end(),
                             [mod_id](const Mod& mod) { return mod.id == mod_id; });
    if(iter != installed_mods_.end() && iter->remote_update_time > iter->install_time)
      iter->suppress_update_time =
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  }
  updateSettings(true);
}

std::string ModdedApplication::getDownloadUrl(const std::string& nxm_url)
{
  return nexus::Api::getDownloadUrl(nxm_url);
}

std::string ModdedApplication::getDownloadUrlForFile(int nexus_file_id, const std::string& mod_url)
{
  return nexus::Api::getDownloadUrl(mod_url, nexus_file_id);
}

std::string ModdedApplication::getNexusPageUrl(const std::string& nxm_url)
{
  return nexus::Api::getNexusPageUrl(nxm_url);
}

std::string ModdedApplication::downloadMod(const std::string& url,
                                           std::function<void(float)> progress_callback)
{
  log_(Log::LOG_DEBUG, "Download URL: " + url);
  std::regex url_regex(R"(.*/(.*)\?.*)");
  std::smatch match;
  if(!std::regex_match(url, match, url_regex))
    throw std::runtime_error(std::format("Invalid download URL \"{}\"", url));
  sfs::path download_path = staging_dir_ / download_dir_;
  if(!sfs::exists(download_path))
    sfs::create_directories(download_path);
  sfs::path file_name = match[1].str();
  const std::string file_name_prefix = file_name.stem();
  const std::string extension = file_name.extension();
  int suffix = 1;
  while(sfs::exists(download_path / file_name))
  {
    file_name = file_name_prefix + "(" + std::to_string(suffix) + ")" + extension;
    suffix++;
  }
  std::string file_name_str = file_name.string();
  auto pos = file_name_str.find("%20");
  while(pos != std::string::npos)
  {
    file_name_str.replace(pos, 3, " ");
    pos = file_name_str.find("%20");
  }
  file_name = file_name_str;

  std::ofstream fstream(download_path / file_name, std::ios::binary);
  if(!fstream.is_open())
    throw std::runtime_error("Failed to write to disk.");
  bool message_sent = false;
  cpr::Response response = cpr::Download(
    fstream,
    cpr::Url(url),
    cpr::ProgressCallback(
      [app = this, &message_sent, &file_name, progress_callback](auto download_total,
                                                                 auto download_now,
                                                                 auto upload_total,
                                                                 auto upload_now,
                                                                 intptr_t user_data)
      {
        if(!message_sent && download_total > 0)
        {
          std::string size_string;
          long last_size = 0;
          long size = download_total;
          int exp = 0;
          const std::vector<std::string> units{ "B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB" };
          while(size > 1024 && exp < units.size())
          {
            last_size = size;
            size /= 1024;
            exp++;
          }
          last_size /= 1.024;
          size_string = std::to_string(size);
          const int first_digit = (last_size / 100) % 10;
          const int second_digit = (last_size / 10) % 10;
          if(first_digit != 0 || second_digit != 0)
            size_string += "." + std::to_string(first_digit);
          if(second_digit != 0)
            size_string += std::to_string(second_digit);
          size_string += units[exp];

          app->log_(Log::LOG_INFO,
                    ("Downloading \"" + file_name.string() + "\" with size: ").c_str() +
                      size_string + "...");
          message_sent = true;
        }
        if(download_total != 0)
          progress_callback((float)download_now / (float)download_total);
        return true;
      }));
  if(response.status_code != 200)
  {
    if(sfs::exists(download_path / file_name))
      sfs::remove(download_path / file_name);
    throw std::runtime_error("Download failed with response: \"" + response.status_line +
                             "\" (code " + std::to_string(response.status_code) + ").");
  }
  fstream.close();
  return (download_path / file_name).string();
}

sfs::path ModdedApplication::iconPath() const
{
  return icon_path_;
}

void ModdedApplication::setIconPath(const sfs::path& icon_path)
{
  icon_path_ = icon_path;
  updateSettings(true);
}

void ModdedApplication::updateSettings(bool write)
{
  json_settings_.clear();
  json_settings_["name"] = name_;
  json_settings_["command"] = command_;
  json_settings_["icon_path"] = icon_path_.string();
  for(int group = 0; group < groups_.size(); group++)
  {
    json_settings_["groups"][group]["active_member"] = active_group_members_[group];
    for(int i = 0; i < groups_[group].size(); i++)
    {
      json_settings_["groups"][group]["members"][i] = groups_[group][i];
    }
  }

  for(int i = 0; i < profile_names_.size(); i++)
    json_settings_["profiles"][i]["name"] = profile_names_[i];

  for(int i = 0; i < app_versions_.size(); i++)
    json_settings_["profiles"][i]["app_version"] = app_versions_[i];

  for(int i = 0; i < installed_mods_.size(); i++)
  {
    json_settings_["installed_mods"][i]["id"] = installed_mods_[i].id;
    json_settings_["installed_mods"][i]["name"] = installed_mods_[i].name;
    json_settings_["installed_mods"][i]["version"] = installed_mods_[i].version;
    json_settings_["installed_mods"][i]["installer"] = installer_map_[installed_mods_[i].id];
    json_settings_["installed_mods"][i]["install_time"] = installed_mods_[i].install_time;
    json_settings_["installed_mods"][i]["local_source"] = installed_mods_[i].local_source.string();
    json_settings_["installed_mods"][i]["remote_source"] = installed_mods_[i].remote_source;
    json_settings_["installed_mods"][i]["remote_update_time"] =
      installed_mods_[i].remote_update_time;
    json_settings_["installed_mods"][i]["size_on_disk"] = installed_mods_[i].size_on_disk;
    json_settings_["installed_mods"][i]["suppress_update_time"] =
      installed_mods_[i].suppress_update_time;
  }

  for(int depl = 0; depl < deployers_.size(); depl++)
  {
    json_settings_["deployers"][depl]["dest_path"] = deployers_[depl]->getDestPath();
    json_settings_["deployers"][depl]["source_path"] = deployers_[depl]->sourcePath().string();
    json_settings_["deployers"][depl]["name"] = deployers_[depl]->getName();
    json_settings_["deployers"][depl]["type"] = deployers_[depl]->getType();
    json_settings_["deployers"][depl]["use_copy_deployment"] =
      deployers_[depl]->usesCopyDeployment();

    if(!deployers_[depl]->isAutonomous())
    {
      for(int prof = 0; prof < profile_names_.size(); prof++)
      {
        deployers_[depl]->setProfile(prof);
        json_settings_["deployers"][depl]["profiles"][prof]["name"] = profile_names_[prof];
        auto loadorder = deployers_[depl]->getLoadorder();
        for(int mod = 0; mod < loadorder.size(); mod++)
        {
          json_settings_["deployers"][depl]["profiles"][prof]["loadorder"][mod]["id"] =
            std::get<0>(loadorder[mod]);
          json_settings_["deployers"][depl]["profiles"][prof]["loadorder"][mod]["enabled"] =
            std::get<1>(loadorder[mod]);
        }
        auto conflict_groups = deployers_[depl]->getConflictGroups();
        for(int group = 0; group < conflict_groups.size(); group++)
        {
          for(int i = 0; i < conflict_groups[group].size(); i++)
            json_settings_["deployers"][depl]["profiles"][prof]["conflict_groups"][group][i] =
              conflict_groups[group][i];
        }
      }
    }
    deployers_[depl]->setProfile(current_profile_);
  }

  for(int tool = 0; tool < tools_.size(); tool++)
  {
    json_settings_["tools"][tool]["name"] = std::get<0>(tools_[tool]);
    json_settings_["tools"][tool]["command"] = std::get<1>(tools_[tool]);
  }

  const auto targets = bak_man_.getTargets();
  for(int i = 0; i < targets.size(); i++)
    json_settings_["backup_targets"][i]["path"] = targets[i].path.string();

  for(int i = 0; i < manual_tags_.size(); i++)
    json_settings_["manual_tags"][i] = manual_tags_[i].toJson();

  for(int i = 0; i < auto_tags_.size(); i++)
  {
    if(!auto_tags_[i].getExpression().empty())
      json_settings_["auto_tags"][i] = auto_tags_[i].toJson();
  }

  if(write)
    writeSettings();
}

void ModdedApplication::writeSettings() const
{
  sfs::path settings_file_path = staging_dir_ / (CONFIG_FILE_NAME + ".tmp");
  std::ofstream file(settings_file_path, std::fstream::binary);
  if(!file.is_open())
    throw std::runtime_error("Error: Could not write to \"" + settings_file_path.string() + "\".");
  file << json_settings_;
  file.close();
  sfs::rename(settings_file_path, staging_dir_ / CONFIG_FILE_NAME);
}

void ModdedApplication::readSettings()
{
  json_settings_.clear();
  sfs::path settings_file_path = staging_dir_ / CONFIG_FILE_NAME;
  std::ifstream file(settings_file_path, std::fstream::binary);
  if(!file.is_open())
    throw std::runtime_error("Error: Could not read from \"" + settings_file_path.string() + "\".");
  file >> json_settings_;
  file.close();
}

void ModdedApplication::updateState(bool read)
{
  installed_mods_.clear();
  deployers_.clear();
  groups_.clear();
  group_map_.clear();
  active_group_members_.clear();
  profile_names_.clear();
  bak_man_.reset();
  tools_.clear();
  profile_names_.clear();
  app_versions_.clear();
  manual_tags_.clear();
  manual_tag_map_.clear();
  auto_tags_.clear();
  auto_tag_map_.clear();
  installer_map_.clear();

  if(read)
  {
    if(!sfs::exists(staging_dir_ / CONFIG_FILE_NAME))
      return;
    readSettings();
  }

  if(!json_settings_.isMember("name"))
    throw ParseError("Name is missing in \"" + (staging_dir_ / CONFIG_FILE_NAME).string() + "\"");
  name_ = json_settings_["name"].asString();

  if(!json_settings_.isMember("command"))
    throw ParseError("Command is missing in \"" + (staging_dir_ / CONFIG_FILE_NAME).string() +
                     "\"");
  command_ = json_settings_["command"].asString();

  if(!json_settings_.isMember("icon_path"))
    throw ParseError("Icon path is missing in \"" + (staging_dir_ / CONFIG_FILE_NAME).string() +
                     "\"");
  icon_path_ = json_settings_["icon_path"].asString();

  if(!json_settings_.isMember("profiles"))
    throw ParseError("Profiles are missing in \"" + (staging_dir_ / CONFIG_FILE_NAME).string() +
                     "\"");

  Json::Value profiles = json_settings_["profiles"];
  for(int i = 0; i < profiles.size(); i++)
  {
    profile_names_.push_back(profiles[i]["name"].asString());
    app_versions_.push_back(profiles[i]["app_version"].asString());
  }

  Json::Value installed_mods = json_settings_["installed_mods"];
  for(int i = 0; i < installed_mods.size(); i++)
  {
    installed_mods_.emplace_back(installed_mods[i]["id"].asInt(),
                                 installed_mods[i]["name"].asString(),
                                 installed_mods[i]["version"].asString(),
                                 installed_mods[i]["install_time"].asInt64(),
                                 installed_mods[i]["local_source"].asString(),
                                 installed_mods[i]["remote_source"].asString(),
                                 installed_mods[i]["remote_update_time"].asInt64(),
                                 installed_mods[i]["size_on_disk"].asInt64(),
                                 installed_mods[i]["suppress_update_time"].asInt64());
    std::string installer = installed_mods[i]["installer"].asString();
    std::vector<std::string> types = Installer::INSTALLER_TYPES;
    if(std::find(types.begin(), types.end(), installer) == types.end())
      throw ParseError("Unknown installer type: " + installer + " in \"" +
                       (staging_dir_ / CONFIG_FILE_NAME).string() + "\"");
    installer_map_[installed_mods[i]["id"].asInt()] = installer;
  }
  Json::Value groups = json_settings_["groups"];
  for(int group = 0; group < groups.size(); group++)
  {
    groups_.push_back(std::vector<int>{});
    for(int i = 0; i < groups[group]["members"].size(); i++)
    {
      int mod_id = groups[group]["members"][i].asInt();
      if(std::find_if(installed_mods_.begin(),
                      installed_mods_.end(),
                      [mod_id](const Mod& m) { return m.id == mod_id; }) == installed_mods_.end())
        throw ParseError("Unknown mod id in group " + std::to_string(group) + ": " +
                         std::to_string(mod_id) + " in \"" +
                         (staging_dir_ / CONFIG_FILE_NAME).string() + "\"");
      if(std::find(groups_[group].begin(), groups_[group].end(), mod_id) != groups_[group].end())
        throw ParseError("Duplicate mod id in group " + std::to_string(group) + ": " +
                         std::to_string(mod_id) + " in \"" +
                         (staging_dir_ / CONFIG_FILE_NAME).string() + "\"");
      group_map_[mod_id] = group;
      groups_[group].push_back(mod_id);
    }
    int active_member = groups[group]["active_member"].asInt();
    if(std::find(groups_[group].begin(), groups_[group].end(), active_member) ==
         groups_[group].end() ||
       !groups[group].isMember("active_member"))
      throw ParseError("Invalid active group member: " + std::to_string(active_member) + " in \"" +
                       (staging_dir_ / CONFIG_FILE_NAME).string() + "\"");
    active_group_members_.push_back(groups[group]["active_member"].asInt());
  }
  Json::Value deployers = json_settings_["deployers"];
  for(int depl = 0; depl < deployers.size(); depl++)
  {
    std::vector<std::string> types = DeployerFactory::DEPLOYER_TYPES;
    std::string type = deployers[depl]["type"].asString();
    if(std::find(types.begin(), types.end(), type) == types.end())
      throw ParseError("Unknown deployer type: " + type + " in \"" +
                       (staging_dir_ / CONFIG_FILE_NAME).string() + "\"");
    deployers_.push_back(
      DeployerFactory::makeDeployer(type,
                                    sfs::path(deployers[depl]["source_path"].asString()),
                                    sfs::path(deployers[depl]["dest_path"].asString()),
                                    deployers[depl]["name"].asString(),
                                    deployers[depl]["use_copy_deployment"].asBool()));
    if(!deployers_[depl]->isAutonomous())
    {
      for(int prof = 0; prof < profile_names_.size(); prof++)
      {
        deployers_[depl]->addProfile();
        deployers_[depl]->setProfile(prof);
        Json::Value loadorder = deployers[depl]["profiles"][prof]["loadorder"];
        for(int mod = 0; mod < loadorder.size(); mod++)
        {
          int mod_id = loadorder[mod]["id"].asInt();
          if(std::find_if(installed_mods_.begin(),
                          installed_mods_.end(),
                          [mod_id](const Mod& m)
                          { return m.id == mod_id; }) == installed_mods_.end())
            throw ParseError("Unknown mod id in deployers: " + std::to_string(mod_id) + " in \"" +
                             (staging_dir_ / CONFIG_FILE_NAME).string() + "\"");
          if(!group_map_.contains(mod_id) || active_group_members_[group_map_[mod_id]] == mod_id &&
                                               !(deployers_[depl]->isAutonomous()))
            deployers_[depl]->addMod(mod_id, loadorder[mod]["enabled"].asBool(), false);
        }
        Json::Value conflict_groups_json = deployers[depl]["profiles"][prof]["conflict_groups"];
        std::vector<std::vector<int>> conflict_groups;
        for(int group = 0; group < conflict_groups_json.size(); group++)
        {
          std::vector<int> new_group;
          for(int mod = 0; mod < conflict_groups_json[group].size(); mod++)
            new_group.push_back(conflict_groups_json[group][mod].asInt());
          conflict_groups.push_back(std::move(new_group));
        }
        deployers_[depl]->setConflictGroups(conflict_groups);
      }
    }
    deployers_[depl]->setProfile(current_profile_);
  }
  Json::Value tools = json_settings_["tools"];
  for(int tool = 0; tool < tools.size(); tool++)
    tools_.emplace_back(tools[tool]["name"].asString(), tools[tool]["command"].asString());

  for(int prof = 0; prof < profile_names_.size(); prof++)
    bak_man_.addProfile();
  bak_man_.setProfile(current_profile_);
  Json::Value backup_targets = json_settings_["backup_targets"];
  for(int target = 0; target < backup_targets.size(); target++)
    bak_man_.addTarget(backup_targets[target]["path"].asString());
  bak_man_.setLog(log_);

  if(json_settings_.isMember("manual_tags"))
  {
    for(auto& tag_entry : json_settings_["manual_tags"])
    {
      if(str::find_if(manual_tags_,
                      [name = tag_entry["name"].asString()](auto tag)
                      { return tag.getName() == name; }) != manual_tags_.end())
        throw ParseError(
          std::format("Manual tag \"{}\" found more than once.", tag_entry["name"].asString()));
      manual_tags_.emplace_back(tag_entry);
    }
    updateManualTagMap();
  }

  if(json_settings_.isMember("auto_tags"))
  {
    for(auto& tag_entry : json_settings_["auto_tags"])
    {
      if(str::find_if(auto_tags_,
                      [name = tag_entry["name"].asString()](auto tag)
                      { return tag.getName() == name; }) != auto_tags_.end())
        throw ParseError(
          std::format("Auto tag \"{}\" found more than once.", tag_entry["name"].asString()));
      auto_tags_.emplace_back(tag_entry);
    }
    updateAutoTagMap();
  }
}

std::string ModdedApplication::getModName(int mod_id) const
{
  auto iter = std::find_if(
    installed_mods_.begin(), installed_mods_.end(), [mod_id](Mod m) { return m.id == mod_id; });
  if(iter == installed_mods_.end())
    return "";
  return iter->name;
}

void ModdedApplication::updateDeployerGroups(std::optional<ProgressNode*> progress_node)
{
  std::vector<std::vector<int>> update_targets;
  for(int depl = 0; depl < deployers_.size(); depl++)
  {
    update_targets.push_back({});
    if(deployers_[depl]->isAutonomous())
      continue;
    for(int profile = 0; profile < profile_names_.size(); profile++)
    {
      deployers_[depl]->setProfile(profile);
      std::vector<bool> completed_groups(active_group_members_.size());
      std::fill(completed_groups.begin(), completed_groups.end(), false);
      for(const auto [mod_id, _] : deployers_[depl]->getLoadorder())
      {
        if(!group_map_.contains(mod_id))
          continue;
        const int group = group_map_[mod_id];
        if(!completed_groups[group])
        {
          completed_groups[group] = true;
          if(deployers_[depl]->swapMod(mod_id, active_group_members_[group]))
            update_targets[depl].push_back(profile);
        }
        else if(deployers_[depl]->removeMod(mod_id))
          update_targets[depl].push_back(profile);
      }
    }
    deployers_[depl]->setProfile(current_profile_);
  }
  if(progress_node)
  {
    std::vector<float> weights;
    for(int depl = 0; depl < update_targets.size(); depl++)
    {
      for(int profile : update_targets[depl])
      {
        deployers_[depl]->setProfile(profile);
        weights.push_back(deployers_[depl]->getNumMods());
      }
      deployers_[depl]->setProfile(current_profile_);
    }
    (*progress_node)->addChildren(weights);
  }
  int i = 0;
  for(int depl = 0; depl < update_targets.size(); depl++)
  {
    for(int profile : update_targets[depl])
    {
      deployers_[depl]->setProfile(profile);
      deployers_[depl]->updateConflictGroups(progress_node ? &(*progress_node)->child(i)
                                                           : std::optional<ProgressNode*>{});
      i++;
    }
    deployers_[depl]->setProfile(current_profile_);
  }
}

void ModdedApplication::splitMod(int mod_id, int deployer)
{
  if(deployers_[deployer]->isAutonomous())
    return;

  std::map<int, sfs::path> managed_sub_dirs;
  for(int i = 0; i < deployers_.size(); i++)
  {
    if(i == deployer || deployers_[i]->isAutonomous())
      continue;
    auto cur_depl_path = deployers_[i]->getDestPath();
    if(!cur_depl_path.ends_with("/"))
      cur_depl_path += "/";
    auto target_depl_path = deployers_[deployer]->getDestPath();
    if(!target_depl_path.ends_with("/"))
      target_depl_path += "/";
    const auto pos = cur_depl_path.find(target_depl_path);
    if(pos != std::string::npos)
    {
      std::string sub_dir = cur_depl_path.substr(pos + target_depl_path.size());
      if(sub_dir.starts_with("/"))
        sub_dir = sub_dir.substr(1);
      managed_sub_dirs[i] = sub_dir;
    }
  }
  if(managed_sub_dirs.empty())
    return;

  for(const auto& [depl, dir] : managed_sub_dirs)
  {
    const auto mod_dir_optional =
      pu::pathExists(dir,
                     staging_dir_ / std::to_string(mod_id),
                     deployers_[deployer]->getType() == DeployerFactory::CASEMATCHINGDEPLOYER);
    if(!mod_dir_optional)
      continue;
    const auto mod_dir = staging_dir_ / std::to_string(mod_id) / mod_dir_optional->string();

    AddModInfo info;
    info.deployers = { depl };
    info.group = -1;
    auto iter =
      str::find_if(installed_mods_, [mod_id](const auto& mod) { return mod.id == mod_id; });
    if(iter == installed_mods_.end())
      throw std::runtime_error(std::format("Invalid mod id {}", mod_id));
    info.name = iter->name + " [" + deployers_[depl]->getName() + "]";
    info.version = iter->version;
    info.installer = Installer::SIMPLEINSTALLER;
    info.installer_flags = Installer::Flag::preserve_case | Installer::Flag::preserve_directories;
    info.files = {};
    info.root_level = 0;
    info.source_path = mod_dir;
    log_(Log::LOG_WARNING,
         std::format("Mod '{}' has been split because it contains"
                     " a sub-directory managed by deployer '{}'.",
                     iter->name,
                     deployers_[depl]->getName()));
    installMod(info);
    if(sfs::exists(mod_dir))
      sfs::remove_all(mod_dir);
  }
}

void ModdedApplication::replaceMod(const AddModInfo& info)
{
  if(!info.replace_mod || info.group == -1)
  {
    installMod(info);
    return;
  }
  auto index =
    str::find_if(installed_mods_, [group = info.group](const Mod& m) { return m.id == group; });
  if(index == installed_mods_.end())
    throw std::runtime_error(std::format("Invalid group '{}' for mod '{}'", info.group, info.name));

  int mod_id = 0;
  if(!installed_mods_.empty())
    mod_id = std::max_element(installed_mods_.begin(), installed_mods_.end())->id + 1;
  while(sfs::exists(staging_dir_ / std::to_string(mod_id)) &&
        mod_id < std::numeric_limits<int>().max())
    mod_id++;
  if(mod_id == std::numeric_limits<int>().max())
    throw std::runtime_error("Error: Could not generate new mod id.");
  const sfs::path tmp_replace_dir =
    staging_dir_ / (std::string("tmp_replace_") + std::to_string(mod_id));

  const auto mod_size = Installer::install(info.source_path,
                                           tmp_replace_dir,
                                           info.installer_flags,
                                           info.installer,
                                           info.root_level,
                                           info.files);
  const sfs::path old_mod_path = staging_dir_ / std::to_string(info.group);
  if(sfs::exists(old_mod_path))
    sfs::remove_all(old_mod_path);
  sfs::rename(tmp_replace_dir, old_mod_path);

  index->name = info.name;
  index->version = info.version;
  index->remote_source = info.remote_source;
  index->local_source = info.local_source;
  index->install_time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  index->remote_update_time = index->install_time;
  index->size_on_disk = mod_size;

  std::vector<float> weights;
  std::vector<std::vector<int>> update_targets;
  for(int depl = 0; depl < deployers_.size(); depl++)
  {
    update_targets.push_back({});
    if(deployers_[depl]->isAutonomous())
      continue;
    for(int prof = 0; prof < profile_names_.size(); prof++)
    {
      deployers_[depl]->setProfile(prof);
      if(deployers_[depl]->hasMod(info.group))
      {
        update_targets[depl].push_back(prof);
        weights.push_back(deployers_[depl]->getNumMods());
      }
    }
    deployers_[depl]->setProfile(current_profile_);
  }
  ProgressNode node(progress_callback_, weights);
  int i = 0;
  for(int depl = 0; depl < update_targets.size(); depl++)
  {
    for(int prof : update_targets[depl])
    {
      deployers_[depl]->setProfile(prof);
      deployers_[depl]->updateConflictGroups(&node.child(i));
      i++;
    }
    deployers_[depl]->setProfile(current_profile_);
  }

  for(auto& tag : auto_tags_)
    tag.updateMods(staging_dir_, std::vector<int>{ info.group });
  updateAutoTagMap();

  updateSettings(true);
}

void ModdedApplication::updateManualTagMap()
{
  manual_tag_map_.clear();
  for(const auto& mod : installed_mods_)
    manual_tag_map_[mod.id] = {};
  for(const auto& tag : manual_tags_)
  {
    for(int mod_id : tag.getMods())
      manual_tag_map_[mod_id].push_back(tag.getName());
  }
}

void ModdedApplication::updateAutoTagMap()
{
  auto_tag_map_.clear();
  for(const auto& mod : installed_mods_)
    auto_tag_map_[mod.id] = {};
  for(const auto& tag : auto_tags_)
  {
    for(int mod_id : tag.getMods())
      auto_tag_map_[mod_id].push_back(tag.getName());
  }
}

void ModdedApplication::performUpdateCheck(const std::vector<int>& target_mod_indices)
{
  if(target_mod_indices.empty())
  {
    log_(Log::LOG_INFO, "None of the selected mods has a valid remote source.");
    return;
  }
  log_(Log::LOG_INFO,
       std::format("Checking for updates for {} mod{}...",
                   target_mod_indices.size(),
                   target_mod_indices.size() > 1 ? "s" : ""));
  ProgressNode node(progress_callback_);
  node.setTotalSteps(target_mod_indices.size());
  int num_available_updates = 0;
  for(int i : target_mod_indices)
  {
    installed_mods_[i].remote_update_time =
      nexus::Api::getNexusPage(installed_mods_[i].remote_source).mod.updated_time;
    if(installed_mods_[i].remote_update_time > installed_mods_[i].install_time)
      num_available_updates++;
    node.advance();
  }
  if(num_available_updates > 0)
    log_(Log::LOG_INFO,
         std::format("Found updates for {} mod{}.",
                     num_available_updates,
                     num_available_updates == 1 ? "" : "s"));
  else
    log_(Log::LOG_INFO, "No mod updates found.");
  updateSettings(true);
}