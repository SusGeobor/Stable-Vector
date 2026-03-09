#pragma once

#include <type_traits>
#include <algorithm>
#include <concepts>
#include <cstdint>

#define NOMINMAX
#include <Windows.h>

// CONCEPTS
struct is_const {};
struct not_const {};
struct use_generations {};
struct no_generations {};


template<class T>
concept Const_Iterator_Concept = std::same_as<T, is_const> || std::same_as<T, not_const>;

template<class T>
concept Use_Generations_Concept = std::same_as<T, use_generations> || std::same_as<T, no_generations>;


template<class, size_t, Use_Generations_Concept, Const_Iterator_Concept>
class stableVectorIterator;

// STABLE VECTOR
template<class T, size_t elements, Use_Generations_Concept UseGenerations = no_generations>
class stableVector
{
private: // TYPES
	struct FreeStackIndex
	{
		uint32_t left;
		uint32_t right;
	};


	struct FreeListNode
	{
		FreeListNode* previous;
		FreeStackIndex indices;
	};


	struct IndividualisticNode
	{
		union
		{
			T value;
			FreeListNode freeListNode;
		};
	};


	struct GenerationalNode
	{
		union
		{
			T value;
			FreeListNode freeListNode;
		};

		size_t generation;
	};

public:
	struct GenerationalHandle
	{
		T* ptr;
		size_t generation;
	};

private: // MEMBER ALIASES
	template<class, size_t, Use_Generations_Concept, Const_Iterator_Concept>
	friend class stableVectorIterator;

	constexpr static bool Generational = std::same_as<UseGenerations, use_generations>;

	using Node = std::conditional_t<Generational, GenerationalNode, IndividualisticNode>;

public:
	using ReturnHandle = std::conditional_t<Generational, GenerationalHandle, T*>;
	using Iterator = stableVectorIterator<T, elements, UseGenerations, not_const>;
	using ConstIterator = stableVectorIterator<T, elements, UseGenerations, is_const>;

public: // CONSTRUCTORS
	stableVector() noexcept
	{
		allocate(1);
	}


	stableVector(size_t reserveElements) noexcept
	{
		allocate(reserveElements);
	}


	stableVector(const stableVector& other) noexcept
	{
		copyStableVector(other);
	}


	stableVector& operator=(const stableVector& other) noexcept
	{
		if (this != &other)
		{
			deallocate();
			highWaterMark = 0;
			freeList = nullptr;

			copyStableVector(other);
		}
		
		return *this;
	}


	stableVector(stableVector&& other) noexcept
	{
		pageCount = other.pageCount;
		highWaterMark = other.highWaterMark;
		data = other.data;
		endData = other.endData;
		skipArray = other.skipArray;
		freeList = other.freeList;

		other.data = nullptr;
		other.skipArray = nullptr;
	}


	stableVector& operator=(stableVector&& other) noexcept
	{
		if (this != &other)
		{
			deallocate();

			pageCount = other.pageCount;
			highWaterMark = other.highWaterMark;
			data = other.data;
			endData = other.endData;
			skipArray = other.skipArray;
			freeList = other.freeList;

			other.data = nullptr;
			other.skipArray = nullptr;
		}

		return *this;
	}


	~stableVector() noexcept
	{
		deallocate();
	}

public: // MEMBER FUNCTIONS, INSERT
	ReturnHandle insert(const T& value) noexcept
	{
		return emplace(value);
	}


	ReturnHandle insert(T&& value) noexcept
	{
		return emplace(std::move(value));
	}


	template<class... Args>
	ReturnHandle emplace(Args&&... args) noexcept
	{
		Node* const slot = getAllocationSlot();

		::new(slot) T(std::forward<Args>(args)...);

		++elementCount;

		if constexpr (Generational)
		{
			++(slot->generation);

			return { reinterpret_cast<T*>(slot), slot->generation };
		}
		else
		{
			return reinterpret_cast<ReturnHandle>(slot);
		}
	}

	// ERASE
	void erase(T* value) noexcept
	{
		const size_t index = reinterpret_cast<Node*>(value) - data;

		data[index].value.~T();
		::new(data + index) FreeListNode(freeList, { skipArray[index], skipArray[index + 2] });
		freeList = reinterpret_cast<FreeListNode*>(data + index);

		if constexpr (Generational)
		{
			++(reinterpret_cast<Node*>(value)->generation);
		}

		updateSkipArray(skipArray + index + 1);

		--elementCount;
	}


	Iterator erase(const Iterator& iterator) noexcept
	{
		Iterator nextElement = iterator;
		++nextElement;

		iterator.data->value.~T();
		::new(iterator.data) FreeListNode(freeList, { *(iterator.skipPtr - 1), *(iterator.skipPtr + 1) });
		freeList = reinterpret_cast<FreeListNode*>(iterator.data);

		if constexpr (Generational)
		{
			++(iterator.data->generation);
		}

		updateSkipArray(iterator.skipPtr);

		--elementCount;

		return nextElement;
	}

	// HELPERS
	[[nodiscard]] size_t size() const noexcept
	{
		return elementCount;
	}


	[[nodiscard]] bool isAlive(T* value) const noexcept
	{
		const size_t index = reinterpret_cast<Node*>(value) - data;

		return !skipArray[index + 1];
	}


	[[nodiscard]] size_t& getGeneration(T* value) noexcept
		requires(Generational)
	{
		return reinterpret_cast<Node*>(value)->generation;
	}


	[[nodiscard]] const size_t& getGeneration(T* value) const noexcept
		requires(Generational)
	{
		return reinterpret_cast<const Node*>(value)->generation;
	}


	void reserve(size_t reserveElements) noexcept
	{
		grow((reserveElements * sizeof(Node)) / pageSize);
	}


	void compress() noexcept
	{
		size_t lastIndex = 0;

		if (elementCount)
		{
			lastIndex = crbegin().data - data;

			for (size_t currentIndex = 0; currentIndex < lastIndex; ++currentIndex)
			{
				if (skipArray[currentIndex + 1])
				{
					::new(data + currentIndex) T(std::move(*reinterpret_cast<T*>(data + lastIndex)));
					reinterpret_cast<T*>(data + lastIndex)->~T();

					if constexpr (Generational)
					{
						++(data[currentIndex].generation);
					}

					while (skipArray[--lastIndex + 1]) {}
				}
			}
		}

		highWaterMark = elementCount;
		freeList = nullptr;

		decommitPages(elementCount);

		for (size_t currentIndex = highWaterMark; data + currentIndex != endData; ++currentIndex)
		{
			::new(data + currentIndex) Node({ 0 });
		}

		memset(skipArray, 0, getSkipArrayBytes());
	}


	void shrinkToFit() noexcept
	{
		size_t index = 0;

		if (elementCount)
		{
			index = crbegin().data - data;
			decommitPages(index);

			FreeListNode* toPointTo = nullptr;
			for (size_t currentIndex = (endData - data) - 1; currentIndex != index; --currentIndex)
			{
				::new(data + currentIndex) Node({ 0 });
				skipArray[currentIndex + 1] = 0;
			}
			for (size_t currentIndex = index; currentIndex != std::numeric_limits<size_t>::max(); --currentIndex)
			{
				if (skipArray[currentIndex + 1])
				{
					data[currentIndex].freeListNode.previous = toPointTo;
					toPointTo = reinterpret_cast<FreeListNode*>(data + currentIndex);
				}
			}

			highWaterMark = index + 1;
			freeList = toPointTo;

			return;
		}

		highWaterMark = 0;
		freeList = nullptr;

		decommitPages(index);

		memset(skipArray, 0, getSkipArrayBytes());
	}

	// ITERATORS
	[[nodiscard]] Iterator begin() noexcept
	{
		return Iterator(data + skipArray[1], skipArray + skipArray[1] + 1);
	}


	[[nodiscard]] Iterator end() noexcept
	{
		return Iterator(data + highWaterMark, skipArray + highWaterMark + 1);
	}


	[[nodiscard]] Iterator rbegin() noexcept
	{
		return --Iterator(data + highWaterMark, skipArray + highWaterMark + 1);
	}


	[[nodiscard]] ConstIterator begin() const noexcept
	{
		return ConstIterator(data + skipArray[1], skipArray + skipArray[1] + 1);
	}


	[[nodiscard]] ConstIterator end() const noexcept
	{
		return ConstIterator(data + highWaterMark, skipArray + highWaterMark + 1);
	}


	[[nodiscard]] ConstIterator rbegin() const noexcept
	{
		return --ConstIterator(data + highWaterMark, skipArray + highWaterMark + 1);
	}


	[[nodiscard]] ConstIterator cbegin() const noexcept
	{
		return begin();
	}


	[[nodiscard]] ConstIterator cend() const noexcept
	{
		return end();
	}


	[[nodiscard]] ConstIterator crbegin() const noexcept
	{
		return rbegin();
	}

private: // IMPLEMENTATION
	void allocate(size_t reserveElements) noexcept
	{
		[[maybe_unused]] static const bool _ = []() noexcept -> bool
		{
			SYSTEM_INFO systemInfo;
			GetSystemInfo(&systemInfo);
			pageSize = static_cast<size_t>(systemInfo.dwPageSize);

			reservedBytes = elements * sizeof(Node);
			skipReservedBytes = (elements + 2) * 4;
			reservedBytes = align(reservedBytes, pageSize);
			skipReservedBytes = align(skipReservedBytes, pageSize);

			return false;
		}();

		size_t reserveSize = reserveElements * sizeof(Node);
		size_t skipReserveSize = (reserveElements + 2) * 4;
		reserveSize = align(reserveSize, pageSize);
		skipReserveSize = align(skipReserveSize, pageSize);

		pageCount = reserveSize / pageSize;

		data = static_cast<Node*>(VirtualAlloc(NULL, reservedBytes, MEM_RESERVE, PAGE_READWRITE));
		skipArray = static_cast<uint32_t*>(VirtualAlloc(NULL, skipReservedBytes, MEM_RESERVE, PAGE_READWRITE));
		VirtualAlloc(data, reserveSize, MEM_COMMIT, PAGE_READWRITE);
		VirtualAlloc(skipArray, skipReserveSize, MEM_COMMIT, PAGE_READWRITE);
		endData = data + (reserveSize / sizeof(Node));
	}


	void copyStableVector(const stableVector& other) noexcept
	{
		allocate(other.elementCount);

		for (const T& element : other)
		{
			::new(data + highWaterMark) T(element);

			if constexpr (Generational)
			{
				++((data + highWaterMark)->generation);
			}

			++highWaterMark;
		}
	}


	void deallocate() noexcept
	{
		if (data)
		{
			for (T& i : *(this))
			{
				i.~T();
			}

			VirtualFree(data, 0, MEM_RELEASE);
			VirtualFree(skipArray, 0, MEM_RELEASE);
		}
	}


	[[nodiscard]] size_t getSkipArrayBytes() const noexcept
	{
		const size_t skipArrayBytes = (((pageCount * pageSize) / sizeof(Node)) + 2) * 4;

		return align(skipArrayBytes, pageSize);
	}


	void decommitPages(size_t index) noexcept
	{
		size_t bytesUsed = (index + 1) * sizeof(Node);
		bytesUsed = align(bytesUsed, pageSize);
		const size_t pagesUsed = bytesUsed / pageSize;
		const size_t bytesToDecommit = (pageCount - pagesUsed) * pageSize;

		const size_t prevSkipArrayBytes = getSkipArrayBytes();

		pageCount = pagesUsed;

		const size_t skipArrayBytes = getSkipArrayBytes();

		VirtualFree(reinterpret_cast<char*>(data) + bytesUsed, bytesToDecommit, MEM_DECOMMIT);
		VirtualFree(reinterpret_cast<char*>(skipArray) + skipArrayBytes, prevSkipArrayBytes - skipArrayBytes, MEM_DECOMMIT);

		endData = data + (bytesUsed / sizeof(Node));
	}


	[[nodiscard]] Node* getAllocationSlot() noexcept
	{
		if (freeList)
		{
			FreeListNode* const prevNode = freeList->previous;
			const uint32_t index = reinterpret_cast<Node*>(freeList) - data;

			rewindSkipArray(index);

			freeList = prevNode;

			return data + index;
		}
		if (data + highWaterMark == endData)
		{
			grow(pageCount * 2);
		}

		++highWaterMark;

		return data + highWaterMark - 1;
	}


	void grow(size_t newPageCount) noexcept
	{
		pageCount = std::clamp(newPageCount, static_cast<size_t>(0), reservedBytes / pageSize);
		endData = data + (pageCount * pageSize / sizeof(Node));

		VirtualAlloc(data, pageCount * pageSize, MEM_COMMIT, PAGE_READWRITE);
		VirtualAlloc(skipArray, getSkipArrayBytes(), MEM_COMMIT, PAGE_READWRITE);
	}


	void rewindSkipArray(uint32_t index) noexcept
	{
		const uint32_t left = freeList->indices.left;
		const uint32_t right = freeList->indices.right;

		skipArray[index] = left;
		skipArray[index + 1 - left] = left;

		skipArray[index + 1] = 0;

		skipArray[index + 2] = right;
		skipArray[index + 1 + right] = right;
	}


	void updateSkipArray(uint32_t* currentSkipNode) noexcept
	{
		if (*(currentSkipNode + 1))
		{
			if (*(currentSkipNode - 1))
			{
				const uint32_t totalSkipSize = *(currentSkipNode + 1) + *(currentSkipNode - 1) + 1;
				const uint32_t rightSkipSize = *(currentSkipNode + 1);
				const uint32_t leftSkipSize = *(currentSkipNode - 1);
				*(currentSkipNode + rightSkipSize) = totalSkipSize;
				*(currentSkipNode - leftSkipSize) = totalSkipSize;
				return;
			}
			const uint32_t newSkipSize = *(currentSkipNode + 1);
			*currentSkipNode = newSkipSize + 1;
			*(currentSkipNode + newSkipSize) = newSkipSize + 1;
		}
		else if (*(currentSkipNode - 1))
		{
			const uint32_t newSkipSize = *(currentSkipNode - 1);
			*currentSkipNode = newSkipSize + 1;
			*(currentSkipNode - newSkipSize) = newSkipSize + 1;
		}
		else
		{
			*currentSkipNode = 1;
		}
	}


	[[nodiscard]] constexpr static size_t align(size_t value, size_t alignment) noexcept
	{
		return (value + alignment - 1) & ~(alignment - 1);
	}

private: // MEMBERS
	static size_t pageSize;
	static size_t reservedBytes;
	static size_t skipReservedBytes;

	size_t pageCount;
	size_t highWaterMark = 0;
	size_t elementCount = 0;
	Node* data;
	Node* endData;
	uint32_t* skipArray;
	FreeListNode* freeList = nullptr;
};

// MEMBER DEFINITIONS
template<class T, size_t elements, Use_Generations_Concept UseGenerations>
size_t stableVector<T, elements, UseGenerations>::reservedBytes;

template<class T, size_t elements, Use_Generations_Concept UseGenerations>
size_t stableVector<T, elements, UseGenerations>::skipReservedBytes;

template<class T, size_t elements, Use_Generations_Concept UseGenerations>
size_t stableVector<T, elements, UseGenerations>::pageSize;

// ITERATOR
template<class T, size_t elements, Use_Generations_Concept UseGenerations, Const_Iterator_Concept IsConst>
class stableVectorIterator
{
private:
	template<class, size_t, Use_Generations_Concept>
	friend class stableVector;

public:
	using value_type = T;
	using difference_type = std::ptrdiff_t;
	using iterator_category = std::bidirectional_iterator_tag;

private:
	constexpr static bool Constant = std::same_as<IsConst, is_const>;

	using ValueType = std::conditional_t<Constant, const T, T>;
	using DataValueType = std::conditional_t<Constant, typename const stableVector<T, elements, UseGenerations>::Node*, typename stableVector<T, elements, UseGenerations>::Node*>;
	using SkipValueType = std::conditional_t<Constant, const uint32_t*, uint32_t*>;

public:
	stableVectorIterator() noexcept = default;


	stableVectorIterator(DataValueType data, SkipValueType skipPtr) noexcept : data(data), skipPtr(skipPtr) {}

public:
	stableVectorIterator& operator++() noexcept
	{
		++skipPtr;
		++data;
		data += *skipPtr;
		skipPtr += *skipPtr;
		return *this;
	}


	stableVectorIterator operator++(int) noexcept
	{
		const stableVectorIterator other{ *this };
		++*this;
		return other;
	}


	stableVectorIterator& operator--() noexcept
	{
		--skipPtr;
		--data;
		data -= *skipPtr;
		skipPtr -= *skipPtr;
		return *this;
	}


	stableVectorIterator operator--(int) noexcept
	{
		const stableVectorIterator other{ *this };
		--*this;
		return other;
	}


	[[nodiscard]] ValueType& operator*() const noexcept
	{
		return data->value;
	}


	[[nodiscard]] ValueType* operator->() const noexcept
	{
		return reinterpret_cast<ValueType*>(data);
	}


	bool operator==(const stableVectorIterator& other) const noexcept { return data == other.data; }
	bool operator!=(const stableVectorIterator& other) const noexcept { return data != other.data; }
	bool operator>(const stableVectorIterator& other) const noexcept { return data > other.data; }
	bool operator<(const stableVectorIterator& other) const noexcept { return data < other.data; }
	bool operator>=(const stableVectorIterator& other) const noexcept { return data >= other.data; }
	bool operator<=(const stableVectorIterator& other) const noexcept { return data <= other.data; }

private:
	DataValueType data;
	SkipValueType skipPtr;
};
