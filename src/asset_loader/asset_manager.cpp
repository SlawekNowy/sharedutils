/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "sharedutils/asset_loader/asset_manager.hpp"
#include "sharedutils/util_path.hpp"
#include <cctype>
#pragma optimize("",off)
static bool contains_dot_file(const std::string_view &key,size_t i)
{
	auto c = key[i];
	if(c == '.')
	{
		if(i == 0 || key[i -1] == '/' || key[i -1] == '\\')
		{
			// '*/.*' or '.*'
			if(i == key.length() -1)
			{
				// '*/.' or '.'
				return true;
			}
			else
			{
				if(key[i +1] == '/' || key[i +1] == '\\')
				{
					// '*/./*' or './*'
					return true;
				}
				else if(key[i +1] == '.')
				{
					// '*/..*' or '..*'
					if(i +1 == key.length() -1)
					{
						// '*/..' or '..'
						return true;
					}
					else if(key[i +2] == '/' || key[i +2] == '\\')
					{
						// '*/../*' or '../*'
						return true;
					}
				}
			}
		}
	}
	return false;
}

// Generates a fast hash for quick lookups
unsigned long hash_identifier(std::string_view key)
{
	// djb2 hash
	unsigned long hash = 5381;
	auto len = key.length();
	size_t offset = 0;
	for(auto i=offset;i<key.length();++i)
	{
		auto idx = i -offset;
		auto c = (unsigned char)key[i];
		if(c == '\\')
			c = '/';
		if(c == '/')
		{
			if(idx == 0)
				continue;
		}
		c = std::tolower(static_cast<unsigned char>(c));
		if(contains_dot_file(key,i))
		{
			// Path contains '.' or '..', we'll have to canonicalize and re-hash
			auto path = util::Path::CreateFile(std::string{key});
			path.Canonicalize();
			auto &strPath = path.GetString();
			return hash_identifier(strPath);
		}

		hash = 33 * hash + c;
	}
	return hash;
}

bool util::Asset::IsInUse() const {return assetObject.use_count() > 1;}

void util::IAssetManager::StripFileExtension(std::string_view &key) const
{
	if(key.empty())
		return;
	int32_t i = key.length() -1;
	while(i >= 0)
	{
		auto c = std::tolower(static_cast<unsigned char>(key[i]));
		if(c == '/' || c == '\\')
			return;
		if(c != '.')
		{
			--i;
			continue;
		}
		auto hash = hash_identifier(key.substr(i +1));
		auto it = std::find_if(m_extensions.begin(),m_extensions.end(),[hash](const FormatExtensionInfo &extInfo) {
			return hash == extInfo.hash;
		});
		if(it != m_extensions.end())
			key = key.substr(0,i);
		return;
	}
}

util::IAssetManager::IAssetManager()
{}
util::IAssetManager::~IAssetManager()
{
	for(auto &assetInfo : m_assets)
	{
		if(!assetInfo.asset)
			continue;
		if(assetInfo.asset->IsInUse())
			std::cout<<"Asset "<<assetInfo.identifier<<" is still in use!"<<std::endl;
	}
}
util::AssetIndex util::IAssetManager::AddToCache(const std::string &assetName,const std::shared_ptr<Asset> &asset)
{
	auto identifier = ToCacheIdentifier(assetName);

	auto hash = GetIdentifierHash(identifier);
	AssetIndex index = 0;
	m_cacheMutex.lock();
	auto it = m_cache.find(hash);
	if(it != m_cache.end())
	{
		index = it->second;
		m_cache[hash] = index;
		m_cacheMutex.unlock();
	}
	else
	{
		index = m_assets.size();
		m_cache[hash] = index;
		m_cacheMutex.unlock();
		m_assets.resize(index +1);
	}
	auto &assetInfo = m_assets[index];
	assetInfo.asset = asset;
	assetInfo.hash = hash;
	assetInfo.identifier = identifier;
	assetInfo.index = index;
	return index;
}
bool util::IAssetManager::RemoveFromCache(const std::string &assetName)
{
	auto identifier = ToCacheIdentifier(assetName);
	auto hash = GetIdentifierHash(identifier);
	std::scoped_lock lock {m_cacheMutex};
	auto it = m_cache.find(hash);
	if(it == m_cache.end())
		return false;
	m_cache.erase(it);
	return true;
}
void util::IAssetManager::RegisterFileExtension(const std::string &ext,AssetFormatType formatType) {m_extensions.push_back({ext,hash_identifier(ext),formatType});}
size_t util::IAssetManager::GetIdentifierHash(const std::string &assetName) const
{
	std::string_view strView {assetName};
	StripFileExtension(strView);
	return hash_identifier(strView);
}
std::string util::IAssetManager::ToCacheIdentifier(const std::string &assetName) const
{
	auto path = util::Path::CreateFile(assetName);
	path.Canonicalize();
	std::string_view v {path.GetString()};
	StripFileExtension(v);
	return std::string{v};
}

bool util::IAssetManager::ClearAsset(AssetIndex idx)
{
	if(idx >= m_assets.size())
		return false;
	auto &assetInfo = m_assets[idx];
	if(!assetInfo.asset)
		return false;
	auto hash = assetInfo.hash;

	m_cacheMutex.lock();
	auto itCache = m_cache.find(hash);
	if(itCache != m_cache.end())
		m_cache.erase(itCache);
	m_cacheMutex.unlock();

	auto itFlag = m_flaggedForDeletion.find(hash);
	if(itFlag != m_flaggedForDeletion.end())
		m_flaggedForDeletion.erase(itFlag);

	assetInfo.asset = nullptr;
	return true;
}

uint32_t util::IAssetManager::ClearUnused()
{
	uint32_t n = 0;
	for(auto &assetInfo : m_assets)
	{
		if(!assetInfo.asset || assetInfo.asset->IsInUse())
			continue;
		if(ClearAsset(assetInfo.index))
			++n;
	}
	return n;
}
uint32_t util::IAssetManager::ClearFlagged()
{
	auto flaggedForDeletion = std::move(m_flaggedForDeletion);
	m_flaggedForDeletion.clear();
	uint32_t n = 0;
	for(auto idx : flaggedForDeletion)
	{
		if(ClearAsset(idx))
			++n;
	}
	return n;
}
uint32_t util::IAssetManager::Clear()
{
	m_cacheMutex.lock();
	auto n = m_cache.size();
	m_cache.clear();
	m_cacheMutex.unlock();

	m_assets.clear();
	m_flaggedForDeletion.clear();
	return n;
}
void util::IAssetManager::FlagForRemoval(const std::string &assetName,bool flag)
{
	auto idx = GetAssetIndex(assetName);
	if(!idx.has_value())
		return;
	FlagForRemoval(*idx,flag);
}

void util::IAssetManager::FlagForRemoval(AssetIndex idx,bool flag)
{
	m_cacheMutex.lock();
	auto it = m_cache.find(idx);
	auto isCached = it != m_cache.end();
	m_cacheMutex.unlock();
	if(!isCached)
		return;
	if(flag)
		m_flaggedForDeletion.insert(idx);
	else
	{
		auto itFlag = m_flaggedForDeletion.find(idx);
		if(itFlag != m_flaggedForDeletion.end())
			m_flaggedForDeletion.erase(itFlag);
	}
}

void util::IAssetManager::FlagAllForRemoval()
{
	m_flaggedForDeletion.reserve(m_assets.size());
	for(auto i=decltype(m_assets.size()){0u};i<m_assets.size();++i)
	{
		auto &assetInfo = m_assets[i];
		if(!assetInfo.asset)
			continue;
		m_flaggedForDeletion.insert(i);
	}
}

util::Asset *util::IAssetManager::GetAsset(AssetIndex index)
{
	return (index < m_assets.size()) ? m_assets[index].asset.get() : nullptr;
}
std::optional<util::AssetIndex> util::IAssetManager::GetAssetIndex(const std::string &assetName) const
{
	auto hash = GetIdentifierHash(assetName);
	std::scoped_lock lock {m_cacheMutex};
	auto it = m_cache.find(hash);
	if(it == m_cache.end())
		return {};
	return it->second;
}

const std::unordered_map<util::AssetIdentifierHash,util::AssetIndex> &util::IAssetManager::GetCache() const {return m_cache;}
const std::vector<util::IAssetManager::AssetInfo> &util::IAssetManager::GetAssets() const {return m_assets;}

const util::Asset *util::IAssetManager::FindCachedAsset(const std::string &assetName) const
{
	return const_cast<IAssetManager*>(this)->FindCachedAsset(assetName);
}
util::Asset *util::IAssetManager::FindCachedAsset(const std::string &assetName)
{
	m_cacheMutex.lock();
	auto it = m_cache.find(GetIdentifierHash(assetName));
	if(it == m_cache.end())
	{
		m_cacheMutex.unlock();
		return nullptr;
	}
	auto idx = it->second;
	m_cacheMutex.unlock();
	return GetAsset(idx);
}
#pragma optimize("",on)