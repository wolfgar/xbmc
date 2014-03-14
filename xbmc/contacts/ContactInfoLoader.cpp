/*
 *      Copyright (C) 2005-2013 Team XBMC
 *      http://www.xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include "ContactInfoLoader.h"
//#include "tags/ContactInfoTag.h"
#include "settings/Settings.h"
#include "FileItem.h"

//using namespace CONTACT_INFO;

CContactInfoLoader::CContactInfoLoader()
{
  m_mapFileItems = new CFileItemList;
}

CContactInfoLoader::~CContactInfoLoader()
{
  StopThread();
  delete m_mapFileItems;
}

void CContactInfoLoader::OnLoaderStart()
{
  // Load previously cached items from HD
  m_mapFileItems->SetPath(m_pVecItems->GetPath());
  m_mapFileItems->Load();
  m_mapFileItems->SetFastLookup(true);

  m_tagReads = 0;
  m_loadTags = CSettings::Get().GetBool("pictures.usetags");

  if (m_pProgressCallback)
    m_pProgressCallback->SetProgressMax(m_pVecItems->GetFileCount());
}

bool CContactInfoLoader::LoadItem(CFileItem* pItem)
{
  if (m_pProgressCallback && !pItem->m_bIsFolder)
    m_pProgressCallback->SetProgressAdvance();
/*
  if (!pItem->IsContact() || pItem->IsZIP() || pItem->IsRAR() || pItem->IsCBR() || pItem->IsCBZ() || pItem->IsInternetStream() || pItem->IsVideo())
    return false;

  if (pItem->HasContactInfoTag())
    return true;

  // first check the cached item
  CFileItemPtr mapItem = (*m_mapFileItems)[pItem->GetPath()];
  if (mapItem && mapItem->m_dateTime==pItem->m_dateTime && mapItem->HasContactInfoTag())
  { // Query map if we previously cached the file on HD
    *pItem->GetContactInfoTag() = *mapItem->GetContactInfoTag();
    pItem->SetArt("thumb", mapItem->GetArt("thumb"));
    return true;
  }

  if (m_loadTags)
  { // Nothing found, load tag from file
    pItem->GetContactInfoTag()->Load(pItem->GetPath());
    m_tagReads++;
  }
*/
  return true;
}

void CContactInfoLoader::OnLoaderFinish()
{
  // cleanup cache loaded from HD
  m_mapFileItems->Clear();

  // Save loaded items to HD
  if (!m_bStop && m_tagReads > 0)
    m_pVecItems->Save();
}
