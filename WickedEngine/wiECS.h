
#ifndef WI_ENTITY_COMPONENT_SYSTEM_H
#define WI_ENTITY_COMPONENT_SYSTEM_H

#include "wiArchive.h"
#include "wiJobSystem.h"

#include <cstdint>
#include <cassert>
#include <vector>
#include <unordered_map>
#include <atomic>
#include <mutex>
#include <algorithm>

#define MAXVAL64 18446744073709551615
//PE: Make sure we always allocate continuous memory blocks.
#define DEFAULT_RESERVED_COUNT 50000

namespace wiECS
{
	using Entity = uint32_t;
	static const Entity INVALID_ENTITY = 0;

	template<typename Component> class ComponentManager;

	class ECSManager
	{
	private:
		std::vector<Entity> m_freeIDs;
		std::atomic<Entity> m_nextID = INVALID_ENTITY + 1;
		std::unordered_map<Entity, size_t> m_componentCounts;
		std::mutex m_mutex;

		uint32_t m_reusedIDCount = 0;

	public:

		inline uint32_t GetReusedIDCount() const
		{
			return m_reusedIDCount;
		}

		inline uint32_t GetCurrentEntityCount() const
		{
			// Get the number of entities currently in use (not in the free list)
			return m_nextID.load() - 1 - (uint32_t)m_freeIDs.size();
		}

		inline Entity CreateEntity()
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (!m_freeIDs.empty())
			{
				Entity id = m_freeIDs.back();
				m_freeIDs.pop_back();
				m_reusedIDCount++;
				return id;
			}
			return m_nextID++;
		}

		inline void OnComponentAdded(Entity entity)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_componentCounts[entity]++;
		}

		inline void OnComponentRemoved(Entity entity)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (m_componentCounts.count(entity) > 0)
			{
				m_componentCounts[entity]--;
				if (m_componentCounts[entity] == 0)
				{
					m_freeIDs.push_back(entity);
					m_componentCounts.erase(entity);
				}
			}
		}

		void clear()
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_freeIDs.clear();
			m_nextID = INVALID_ENTITY + 1;
			m_componentCounts.clear();
			m_reusedIDCount = 0;
		}
	};

	extern ECSManager ecs;

	inline uint32_t GetReusedEntityIDs()
	{
		return ecs.GetReusedIDCount();
	}

	inline uint32_t GetCurrentEntityCount()
	{
		return ecs.GetCurrentEntityCount();
	}

	inline Entity CreateEntity()
	{
		return ecs.CreateEntity();
	}

	struct EntitySerializer
	{
		wiJobSystem::context ctx;
		std::unordered_map<uint64_t, Entity> remap;
		bool allow_remap = true;

		~EntitySerializer()
		{
			wiJobSystem::Wait(ctx);
		}
	};

	inline void SerializeEntity(wiArchive& archive, Entity& entity, EntitySerializer& seri)
	{
		if (archive.IsReadMode())
		{
			uint64_t mem;
			archive >> mem;

			if (seri.allow_remap)
			{
				auto it = seri.remap.find(mem);
				if (it == seri.remap.end())
				{
					entity = CreateEntity();
					seri.remap[mem] = entity;
				}
				else
				{
					entity = it->second;
				}
			}
			else
			{
				entity = (Entity)mem;
			}
		}
		else
		{
			archive << entity;
		}
	}

	template<typename Component>
	class ComponentManager
	{
	public:
		ComponentManager(size_t reservedCount = DEFAULT_RESERVED_COUNT)
		{
			components.reserve(reservedCount);
			entities.reserve(reservedCount);
			sparse.resize(reservedCount, ~0);
		}
		ComponentManager()
		{
		}

		inline void Clear()
		{
			for (Entity entity : entities)
			{
				ecs.OnComponentRemoved(entity);
			}
			components.clear();
			entities.clear();
			sparse.assign(sparse.size(), ~0);
		}

		inline void Copy(const ComponentManager<Component>& other)
		{
			Clear();
			for (const Entity& entity : other.entities)
			{
				ecs.OnComponentAdded(entity);
			}
			components = other.components;
			entities = other.entities;
			sparse = other.sparse;
		}

		inline void Merge(ComponentManager<Component>& other)
		{
			components.reserve(GetCount() + other.GetCount());
			entities.reserve(GetCount() + other.GetCount());
			if (sparse.size() < other.sparse.size())
			{
				//PE: Make sure we always allocate continuous memory blocks
				sparse.resize(other.sparse.size() + 5000, ~0);
			}
			for (size_t i = 0; i < other.GetCount(); ++i)
			{
				Entity entity = other.entities[i];
				assert(!Contains(entity));
				entities.push_back(entity);
				if (entity >= sparse.size())
				{
					//PE: Make sure we always allocate continuous memory blocks
					sparse.resize(entity + 5000, ~0);
				}
				sparse[entity] = components.size();
				components.push_back(std::move(other.components[i]));
				ecs.OnComponentAdded(entity);
			}
			other.Clear();
		}

		inline void Serialize(wiArchive& archive, EntitySerializer& seri)
		{
			if (archive.IsReadMode())
			{
				Clear();
				size_t count;
				archive >> count;
				components.resize(count);
				for (size_t i = 0; i < count; ++i)
				{
					components[i].Serialize(archive, seri);
				}
				entities.resize(count);
				size_t max_entity = 0;
				for (size_t i = 0; i < count; ++i)
				{
					Entity entity;
					SerializeEntity(archive, entity, seri);
					entities[i] = entity;
					if (entity > max_entity)
					{
						max_entity = entity;
					}
					ecs.OnComponentAdded(entity);
				}
				if (max_entity + 1 > sparse.size()) {
					//PE: Make sure we always allocate continuous memory blocks
					sparse.resize(max_entity + 5000, ~0);
				}
				for (size_t i = 0; i < count; ++i)
				{
					sparse[entities[i]] = i;
				}
			}
			else
			{
				archive << components.size();
				for (Component& component : components)
				{
					component.Serialize(archive, seri);
				}
				for (Entity entity : entities)
				{
					SerializeEntity(archive, entity, seri);
				}
			}
		}

		inline Component& Create(Entity entity)
		{
			assert(entity != INVALID_ENTITY);
			if (entity >= sparse.size())
			{
				//PE: Make sure we always allocate continuous memory blocks
				sparse.resize(entity + 5000, ~0);
			}
			assert(sparse[entity] == ~0);

			sparse[entity] = components.size();
			components.emplace_back();
			entities.push_back(entity);

			ecs.OnComponentAdded(entity);
			return components.back();
		}

		inline void Remove(Entity entity)
		{
			if (entity < sparse.size() && sparse[entity] != ~0)
			{
				const size_t index = sparse[entity];
				const Entity entity_to_remove = entities[index];

				if (index < components.size() - 1)
				{
					components[index] = std::move(components.back());
					entities[index] = entities.back();
					if (entities[index] < sparse.size()) {
						sparse[entities[index]] = index;
					}
				}
				components.pop_back();
				entities.pop_back();
				if (entity_to_remove < sparse.size()) {
					sparse[entity_to_remove] = ~0;
				}

				ecs.OnComponentRemoved(entity_to_remove);
			}
		}

		inline void Remove_KeepSorted(Entity entity)
		{
			if (entity < sparse.size() && sparse[entity] != ~0)
			{
				const size_t index = sparse[entity];
				const Entity entity_to_remove = entities[index];

				if (index < components.size() - 1)
				{
					for (size_t i = index + 1; i < components.size(); ++i)
					{
						components[i - 1] = std::move(components[i]);
					}
					for (size_t i = index + 1; i < entities.size(); ++i)
					{
						entities[i - 1] = entities[i];
						if (entities[i - 1] < sparse.size()) {
							sparse[entities[i - 1]] = i - 1;
						}
					}
				}

				components.pop_back();
				entities.pop_back();
				if (entity_to_remove < sparse.size()) {
					sparse[entity_to_remove] = ~0;
				}
				ecs.OnComponentRemoved(entity_to_remove);
			}
		}

		inline void MoveItem(size_t index_from, size_t index_to)
		{
			assert(index_from < GetCount());
			assert(index_to < GetCount());
			if (index_from == index_to)
			{
				return;
			}
			Component component = std::move(components[index_from]);
			Entity entity = entities[index_from];

			const int direction = index_from < index_to ? 1 : -1;
			for (size_t i = index_from; i != index_to; i += direction)
			{
				const size_t next = i + direction;
				components[i] = std::move(components[next]);
				entities[i] = entities[next];
				if (entities[i] < sparse.size()) {
					sparse[entities[i]] = i;
				}
			}
			components[index_to] = std::move(component);
			entities[index_to] = entity;
			if (entity < sparse.size()) {
				sparse[entity] = index_to;
			}
		}

		inline bool Contains(Entity entity) const
		{
			if (entity >= sparse.size())
			{
				return false;
			}
			return sparse[entity] != ~0;
		}

		inline Component* GetComponent(Entity entity)
		{
			if (entity < sparse.size() && sparse[entity] != ~0)
			{
				return &components[sparse[entity]];
			}
			return nullptr;
		}

		inline const Component* GetComponent(Entity entity) const
		{
			if (entity < sparse.size() && sparse[entity] != ~0)
			{
				return &components[sparse[entity]];
			}
			return nullptr;
		}

		inline size_t GetIndex(Entity entity) const
		{
			if (entity < sparse.size())
			{
				return sparse[entity];
			}
			return ~0;
		}

		inline size_t GetCount() const { return components.size(); }
		inline size_t GetEntitiesCount() const { return entities.size(); }
		inline size_t GetSparseCount() const { return sparse.size(); }
		inline Entity GetEntity(size_t index) const { return entities[index]; }
		inline const std::vector<Entity>& GetEntities() const { return entities; }
		inline Component& operator[](size_t index) { return components[index]; }
		inline const Component& operator[](size_t index) const { return components[index]; }

	private:
		std::vector<Component> components;
		std::vector<Entity> entities;
		std::vector<size_t> sparse;
		ComponentManager(const ComponentManager&) = delete;
	};
}

#endif // WI_ENTITY_COMPONENT_SYSTEM_H
