#include "deployerlistmodel.h"
#include "colors.h"
#include "modlistmodel.h"
#include <QApplication>
#include <QBrush>
#include <QDebug>
#include <ranges>

namespace str = std::ranges;


DeployerListModel::DeployerListModel(QObject* parent) : QAbstractTableModel(parent) {}

QVariant DeployerListModel::headerData(int section, Qt::Orientation orientation, int role) const
{
  if(role == Qt::TextAlignmentRole && section == 2)
    return Qt::AlignLeft;
  if(role == Qt::DisplayRole)
  {
    if(orientation == Qt::Orientation::Vertical)
      return QString::number(section + 1);
    if(section == status_col)
      return QString("Status");
    if(section == name_col)
      return QString("Name");
    if(section == id_col)
      return QString("ID");
    if(section == tags_col)
      return QString("Tags");
  }
  return QVariant();
}

int DeployerListModel::rowCount(const QModelIndex& parent) const
{
  return deployer_info_.loadorder.size();
}

int DeployerListModel::columnCount(const QModelIndex& parent) const
{
  return 4;
}

QVariant DeployerListModel::data(const QModelIndex& index, int role) const
{
  const int row = index.row();
  const int col = index.column();
  if(role == Qt::BackgroundRole)
  {
    if(col == status_col)
      return QBrush(std::get<1>(deployer_info_.loadorder[row]) ? colors::GREEN : colors::GRAY);
  }
  if(role == Qt::ForegroundRole)
  {
    if(col == status_col)
      return QBrush(QColor(255, 255, 255));
    if(!text_colors_.contains(std::get<0>(deployer_info_.loadorder[row])))
      return QApplication::palette().text();
    return text_colors_.at(std::get<0>(deployer_info_.loadorder[row]));
  }
  if(role == Qt::TextAlignmentRole && col == status_col)
    return Qt::AlignCenter;
  if(role == Qt::DisplayRole)
  {
    if(col == status_col)
      return QString(std::get<1>(deployer_info_.loadorder[row]) ? "Enabled" : "Disabled");
    if(col == name_col)
    {
      return QString::fromStdString(deployer_info_.mod_names[row]);
    }
    if(col == id_col)
      return std::get<0>(deployer_info_.loadorder[row]);
    if(col == tags_col)
    {
      QStringList tags;
      for(const auto& tag : tags_.at(row))
        tags.append(tag.c_str());
      tags.sort(Qt::CaseInsensitive);
      return tags.join(", ");
    }
  }
  if(role == mod_status_role)
    return std::get<1>(deployer_info_.loadorder[row]);
  if(role == ModListModel::mod_id_role)
    return std::get<0>(deployer_info_.loadorder[row]);
  if(role == ModListModel::mod_name_role)
    return deployer_info_.mod_names[row].c_str();
  if(role == mod_tags_role)
  {
    QStringList tags;
    for(const auto& tag : tags_.at(row))
      tags.append(tag.c_str());
    return tags;
  }
  return QVariant();
}

void DeployerListModel::setDeployerInfo(const DeployerInfo& info)
{
  emit layoutAboutToBeChanged();
  tags_.clear();
  if(info.manual_tags.size() == 0)
  {
    for(const auto& tag : info.auto_tags)
      tags_.push_back(tag);
  }
  else
  {
    for(const auto& [man_tags, auto_tags] : str::zip_view(info.manual_tags, info.auto_tags))
    {
      std::vector<std::string> all_tags = man_tags;
      all_tags.insert(all_tags.end(), auto_tags.begin(), auto_tags.end());
      tags_.push_back(all_tags);
    }
  }
  deployer_info_ = info;
  for(int group = 0; group < info.conflict_groups.size(); group++)
  {
    for(int mod_id : info.conflict_groups[group])
    {
      QBrush color(group % 2 == 0 ? colors::LIGHT_BLUE : colors::ORANGE);
      if(group == info.conflict_groups.size() - 1)
        color = QApplication::palette().text();
      text_colors_[mod_id] = color;
    }
  }
  emit layoutChanged();
}