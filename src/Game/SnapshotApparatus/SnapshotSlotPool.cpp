#include "SnapshotSlotPool.h"

#include "Core/logger.h"

#include <mutex>
#include <string>

namespace
{
	std::mutex g_mutex;
	const char* g_owners[SnapshotSlotPool::kSlotCount] = {};
}

int SnapshotSlotPool::Acquire(const char* owner, int count)
{
	if (count <= 0 || count > kSlotCount)
	{
		LOG(2, "[SnapshotSlots] rejected request owner=%s count=%d\n", owner ? owner : "?", count);
		return -1;
	}

	std::lock_guard<std::mutex> lock(g_mutex);

	for (int base = 0; base + count <= kSlotCount; ++base)
	{
		bool freeRange = true;
		for (int i = 0; i < count; ++i)
		{
			if (g_owners[base + i] != nullptr)
			{
				freeRange = false;
				break;
			}
		}
		if (!freeRange)
		{
			continue;
		}

		for (int i = 0; i < count; ++i)
		{
			g_owners[base + i] = owner;
		}
		LOG(2, "[SnapshotSlots] %s reserved %d slot(s) at %d\n", owner ? owner : "?", count, base);
		return base;
	}

	// Not fatal: the caller falls back to sharing the ring, which is what every consumer did
	// before this pool existed.
	LOG(2, "[SnapshotSlots] ring full, %s could not reserve %d slot(s)\n", owner ? owner : "?", count);
	return -1;
}

void SnapshotSlotPool::Release(int base, int count)
{
	if (base < 0 || count <= 0)
	{
		return;
	}

	std::lock_guard<std::mutex> lock(g_mutex);
	for (int i = 0; i < count && (base + i) < kSlotCount; ++i)
	{
		g_owners[base + i] = nullptr;
	}
	LOG(2, "[SnapshotSlots] released %d slot(s) at %d\n", count, base);
}

std::string SnapshotSlotPool::DescribeUsage()
{
	std::lock_guard<std::mutex> lock(g_mutex);

	std::string description;
	for (int i = 0; i < kSlotCount; ++i)
	{
		if (!description.empty())
		{
			description += ' ';
		}
		description += std::to_string(i);
		description += ':';
		description += g_owners[i] ? g_owners[i] : "-";
	}
	return description;
}
