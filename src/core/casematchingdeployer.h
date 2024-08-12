/*!
 * \file casematchingdeployer.h
 * \brief Header for the CaseMatchingDeployer class
 */

#pragma once

#include "deployer.h"

/*!
 * \brief Automatically renames mod files to match the case of target files.
 */
class CaseMatchingDeployer : public Deployer
{
public:
  /*!
   * \brief Passes arguments to base class constructor.
   * \param source_path Path to directory containing mods installed using the Installer class.
   * \param dest_path Path to target directory for mod deployment.
   * \param name A custom name for this instance.
   * \param use_copy_deployment If True: copy files during deployment, else use hard links.
   */
  CaseMatchingDeployer(const std::filesystem::path& source_path,
                       const std::filesystem::path& dest_path,
                       const std::string& name,
                       bool use_copy_deployment = false);
  /*!
   * \brief Iterates over every file and directory contained in the mods in the given load order.
   * If any name case insensitively matches the name of a file in the target directory, the source
   * is renamed to be identical to the target. Then calls
   * \ref Deployer.deploy() "Deployer::deploy(loadorder)".
   * \param loadorder A vector of mod ids representing the load order.
   * \param progress_node Used to inform about the current progress of deployment.
   * \return A map from deployed mod ids to their respective mods total size on disk.
   */
  virtual std::map<int, unsigned long> deploy(
    const std::vector<int>& loadorder,
    std::optional<ProgressNode*> progress_node = {}) override;
  /*! \brief Use base class implementation of overloaded function. */
  using Deployer::deploy;

private:
  /*!
   * \brief Recursively renames every file in source_path_/mod_id/path to the name of a file
   * in dest_path_, if both match case insensitively.
   * \param path Path relative to the mods root directory.
   * \param mod_id Id of the mod containing the source files.
   * \param target_path Path used for file comparisons.
   */
  void adaptDirectoryFiles(const std::filesystem::path& path,
                           int mod_id,
                           const std::filesystem::path& target_path) const;
  /*!
   * \brief Renames every file in every mod in the given load order
   * such that all paths are case invariant and match the case of files in \ref dest_path_.
   * \param loadorder Contains ids of mods the files of which will be adapted.
   * \param progress_node Used to inform about the current progress of deployment.
   */
  void adaptLoadorderFiles(const std::vector<int>& loadorder,
                           std::optional<ProgressNode*> progress_node = {}) const;
};