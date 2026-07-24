#pragma once

#include <cstdint>
#include <vector>

// Metal resources cannot travel through render::frame's 32-bit handles as
// pointers. Handles are {generation:8, slot:24}; zero is permanently invalid.
template <class T> class HandleRegistry
{
  public:
    using Handle = std::uint32_t;

    Handle Allocate(T* value)
    {
        std::uint32_t index = 0;
        if (_free.empty())
        {
            index = static_cast<std::uint32_t>(_slots.size());
            if (index >= kIndexMask)
                return 0;
            _slots.push_back({});
        }
        else
        {
            index = _free.back();
            _free.pop_back();
        }

        Slot& slot = _slots[index];
        if (slot.generation == 0)
            slot.generation = 1;
        slot.value = value;
        slot.live = true;
        return (static_cast<Handle>(slot.generation) << kGenerationShift) | (index + 1);
    }

    T* Resolve(Handle handle) const
    {
        if (handle == 0)
            return nullptr;
        const std::uint32_t encodedIndex = handle & kIndexMask;
        if (encodedIndex == 0)
            return nullptr;
        const std::uint32_t index = encodedIndex - 1;
        if (index >= _slots.size())
            return nullptr;
        const Slot& slot = _slots[index];
        const std::uint8_t generation = static_cast<std::uint8_t>(handle >> kGenerationShift);
        return slot.live && slot.generation == generation ? slot.value : nullptr;
    }

    bool Replace(Handle handle, T* value)
    {
        if (!Resolve(handle))
            return false;
        _slots[(handle & kIndexMask) - 1].value = value;
        return true;
    }

    bool Release(Handle handle)
    {
        if (!Resolve(handle))
            return false;
        const std::uint32_t index = (handle & kIndexMask) - 1;
        Slot& slot = _slots[index];
        slot.value = nullptr;
        slot.live = false;
        ++slot.generation;
        if (slot.generation == 0)
            slot.generation = 1;
        _free.push_back(index);
        return true;
    }

    void Clear()
    {
        _slots.clear();
        _free.clear();
    }

  private:
    static constexpr std::uint32_t kGenerationShift = 24;
    static constexpr std::uint32_t kIndexMask = 0x00ffffffu;

    struct Slot
    {
        T* value = nullptr;
        std::uint8_t generation = 1;
        bool live = false;
    };

    std::vector<Slot> _slots;
    std::vector<std::uint32_t> _free;
};
