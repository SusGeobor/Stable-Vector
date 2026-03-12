#pragma once

#include <type_traits>
#include <algorithm>
#include <iostream>
#include <concepts>
#include <cstdint>
#include <bit>
#include <new>

#define NOMINMAX
#include <Windows.h>


namespace gbr
{
	// CONCEPTS
	struct is_const {};
	struct not_const {};
	struct use_generations {};
	struct no_generations {};


	template<class T>
	concept Const_Iterator_Concept = std::same_as<T, is_const> || std::same_as<T, not_const>;

	template<class T>
	concept Use_Generations_Concept = std::same_as<T, use_generations> || std::same_as<T, no_generations>;


	class OS_PAGE_INFO;

	template<class, size_t, Use_Generations_Concept, Const_Iterator_Concept>
	class stableVectorIterator;

	template<class KeyValue>
	class RemapMap;

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
		stableVector()
		{
			allocate(1);
		}


		stableVector(size_t reserveElements)
		{
			allocate(reserveElements);
		}


		stableVector(const stableVector& other)
		{
			copyStableVector(other);
		}


		stableVector& operator=(const stableVector& other)
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
			elementCount = other.elementCount;
			data = other.data;
			endData = other.endData;
			skipArray = other.skipArray;
			freeList = other.freeList;

			other.elementCount = 0;
			other.data = nullptr;
			other.skipArray = nullptr;
		}


		stableVector& operator=(stableVector&& other) noexcept(std::is_nothrow_destructible_v<T>)
		{
			if (this != &other)
			{
				deallocate();

				pageCount = other.pageCount;
				highWaterMark = other.highWaterMark;
				elementCount = other.elementCount;
				data = other.data;
				endData = other.endData;
				skipArray = other.skipArray;
				freeList = other.freeList;

				other.elementCount = 0;
				other.data = nullptr;
				other.skipArray = nullptr;
			}

			return *this;
		}


		~stableVector() noexcept(std::is_nothrow_destructible_v<T>)
		{
			deallocate();
		}

	public: // MEMBER FUNCTIONS, INSERT
		ReturnHandle insert(const T& value)
		{
			return emplace(value);
		}


		ReturnHandle insert(T&& value)
		{
			return emplace(std::move(value));
		}


		template<class... Args>
		ReturnHandle emplace(Args&&... args)
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
		void erase(T* value) noexcept(std::is_nothrow_destructible_v<T>)
		{
			const size_t index = reinterpret_cast<Node*>(value) - data;

			data[index].value.~T();
			::new(data + index) FreeListNode{ freeList, { skipArray[index], skipArray[index + 2] } };
			freeList = reinterpret_cast<FreeListNode*>(data + index);

			if constexpr (Generational)
			{
				++(reinterpret_cast<Node*>(value)->generation);
			}

			updateSkipArray(skipArray + index + 1);

			--elementCount;
		}


		Iterator erase(const Iterator& iterator) noexcept(std::is_nothrow_destructible_v<T>)
		{
			Iterator nextElement = iterator;
			++nextElement;

			iterator.data->value.~T();
			::new(iterator.data) FreeListNode{ freeList, { iterator.skipPtr[-1], iterator.skipPtr[1] } };
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


		[[nodiscard]] static bool isValid(T* value, size_t generation) noexcept
			requires Generational
		{
			return reinterpret_cast<Node*>(value)->generation == generation;
		}


		[[nodiscard]] static size_t& getGeneration(T* value) noexcept
			requires Generational
		{
			return reinterpret_cast<Node*>(value)->generation;
		}


		[[nodiscard]] static const size_t& getGeneration(const T* value) noexcept
			requires Generational
		{
			return reinterpret_cast<const Node*>(value)->generation;
		}


		void reserve(size_t reserveElements)
		{
			if (const size_t newPageCount = align(reserveElements * sizeof(Node), OS_PAGE_INFO::pageSize) / OS_PAGE_INFO::pageSize; newPageCount > pageCount)
			{
				grow(newPageCount);
			}
		}


		[[nodiscard]] RemapMap<T*> compress() // Intended to break pointer stability
		{
			RemapMap<T*> remapMap;

			size_t lastIndex = 0;

			if (elementCount)
			{
				lastIndex = cback().data - data;

				remapMap.allocate(std::bit_ceil((lastIndex + 1) - elementCount) * 2);

				for (size_t currentIndex = 0; currentIndex < lastIndex; ++currentIndex)
				{
					if (skipArray[currentIndex + 1])
					{
						::new(data + currentIndex) T(std::move(*reinterpret_cast<T*>(data + lastIndex)));

						if constexpr (Generational)
						{
							data[currentIndex].generation = data[lastIndex].generation;
						}

						reinterpret_cast<T*>(data + lastIndex)->~T();

						remapMap.insert(reinterpret_cast<T*>(data + lastIndex), reinterpret_cast<T*>(data + currentIndex));

						while (skipArray[--lastIndex + 1] && lastIndex > currentIndex) {}
					}
				}

				decommitPages(elementCount - 1);
			}
			else
			{
				decommitPages(lastIndex);
			}

			highWaterMark = elementCount;
			freeList = nullptr;

			if constexpr (Generational)
			{
				for (size_t currentIndex = highWaterMark; data + currentIndex != endData; ++currentIndex)
				{
					::new(data + currentIndex) Node{};
				}
			}

			memset(skipArray, 0, getSkipArrayBytes());

			return remapMap;
		}


		void shrinkToFit() noexcept
		{
			size_t index = 0;

			if (elementCount)
			{
				index = cback().data - data;

				decommitPages(index);

				FreeListNode* toPointTo = nullptr;
				for (size_t currentIndex = (endData - data) - 1; currentIndex != index; --currentIndex)
				{
					if constexpr (Generational)
					{
						::new(data + currentIndex) Node{};
					}

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

			decommitPages(index);

			highWaterMark = 0;
			freeList = nullptr;

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


		[[nodiscard]] Iterator back() noexcept
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


		[[nodiscard]] ConstIterator back() const noexcept
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


		[[nodiscard]] ConstIterator cback() const noexcept
		{
			return back();
		}

	private: // IMPLEMENTATION
		void allocate(size_t reserveElements)
		{
			[[maybe_unused]] static const bool _ = []() noexcept -> bool
				{
					OS_PAGE_INFO::getSystemPageData();

					reservedBytes = elements * sizeof(Node);
					skipReservedBytes = (elements + 2) * sizeof(uint32_t);
					reservedBytes = align(reservedBytes, OS_PAGE_INFO::pageSize);
					skipReservedBytes = align(skipReservedBytes, OS_PAGE_INFO::pageSize);

					return false;
				}();

			reserveElements = std::clamp(reserveElements, 1ULL, elements);

			size_t reserveSize = reserveElements * sizeof(Node);
			size_t skipReserveSize = (reserveElements + 2) * sizeof(uint32_t);
			reserveSize = align(reserveSize, OS_PAGE_INFO::pageSize);
			skipReserveSize = align(skipReserveSize, OS_PAGE_INFO::pageSize);

			pageCount = reserveSize / OS_PAGE_INFO::pageSize;

			data = static_cast<Node*>(VirtualAlloc(NULL, reservedBytes, MEM_RESERVE, PAGE_READWRITE));
			skipArray = static_cast<uint32_t*>(VirtualAlloc(NULL, skipReservedBytes, MEM_RESERVE, PAGE_READWRITE));

			if (!data || !skipArray)
			{
				goto FAIL;
			}
			if (!VirtualAlloc(data, reserveSize, MEM_COMMIT, PAGE_READWRITE) ||
				!VirtualAlloc(skipArray, skipReserveSize, MEM_COMMIT, PAGE_READWRITE))
			{
				goto FAIL;
			}

			endData = data + (reserveSize / sizeof(Node));

			return;

		FAIL:
			throw std::bad_alloc();
		}


		void copyStableVector(const stableVector& other)
		{
			elementCount = other.elementCount;

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


		void deallocate() noexcept(std::is_nothrow_destructible_v<T>)
		{
			if (data)
			{
				for (T& item : *(this))
				{
					item.~T();
				}

				if (!VirtualFree(data, 0, MEM_RELEASE) ||
					!VirtualFree(skipArray, 0, MEM_RELEASE))
				{
					std::cerr << "VirtualFree failed\n";
					std::terminate();
				}
			}
		}


		[[nodiscard]] size_t getSkipArrayBytes() const noexcept
		{
			const size_t skipArrayBytes = (((pageCount * OS_PAGE_INFO::pageSize) / sizeof(Node)) + 2) * sizeof(uint32_t);

			return align(skipArrayBytes, OS_PAGE_INFO::pageSize);
		}


		void decommitPages(size_t index) noexcept
		{
			size_t bytesUsed = (index + 1) * sizeof(Node);
			bytesUsed = align(bytesUsed, OS_PAGE_INFO::pageSize);
			const size_t pagesUsed = bytesUsed / OS_PAGE_INFO::pageSize;
			const size_t bytesToDecommit = (pageCount - pagesUsed) * OS_PAGE_INFO::pageSize;

			const size_t prevSkipArrayBytes = getSkipArrayBytes();

			pageCount = pagesUsed;

			const size_t skipArrayBytes = getSkipArrayBytes();
			const size_t skipArrayBytesToDecommit = (prevSkipArrayBytes - skipArrayBytes);

			if (bytesToDecommit)
			{
				if (!VirtualFree(reinterpret_cast<char*>(data) + bytesUsed, bytesToDecommit, MEM_DECOMMIT))
				{
					goto FAIL;
				}
			}
			if (skipArrayBytesToDecommit)
			{
				if (!VirtualFree(reinterpret_cast<char*>(skipArray) + skipArrayBytes, skipArrayBytesToDecommit, MEM_DECOMMIT))
				{
					goto FAIL;
				}
			}

			endData = data + (bytesUsed / sizeof(Node));

			return;

		FAIL:
			std::cerr << "VirtualFree failed, Error Code: " << GetLastError() << '\n';
			std::terminate();
		}


		[[nodiscard]] Node* getAllocationSlot()
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


		void grow(size_t newPageCount)
		{
			pageCount = std::min(newPageCount, reservedBytes / OS_PAGE_INFO::pageSize);
			endData = data + (pageCount * OS_PAGE_INFO::pageSize / sizeof(Node));

			if (!VirtualAlloc(data, pageCount * OS_PAGE_INFO::pageSize, MEM_COMMIT, PAGE_READWRITE) ||
				!VirtualAlloc(skipArray, getSkipArrayBytes(), MEM_COMMIT, PAGE_READWRITE))
			{
				throw std::bad_alloc();
			}
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


		void updateSkipArray(uint32_t* currentSkipNodePtr) noexcept
		{
			const uint32_t leftSkipSize = currentSkipNodePtr[-1];
			uint32_t& currentSkipNode = currentSkipNodePtr[0];
			const uint32_t rightSkipSize = currentSkipNodePtr[1];

			if (leftSkipSize)
			{
				if (rightSkipSize)
				{
					const uint32_t newSkipSize = leftSkipSize + rightSkipSize + 1;

					currentSkipNodePtr[rightSkipSize] = newSkipSize;
					*(currentSkipNodePtr - leftSkipSize) = newSkipSize;

					return;
				}

				const uint32_t newSkipSize = leftSkipSize;

				currentSkipNode = newSkipSize + 1;
				*(currentSkipNodePtr - newSkipSize) = newSkipSize + 1;
			}
			else if (rightSkipSize)
			{
				const uint32_t newSkipSize = rightSkipSize;

				currentSkipNode = newSkipSize + 1;
				currentSkipNodePtr[newSkipSize] = newSkipSize + 1;
			}
			else
			{
				currentSkipNode = 1;
			}
		}


		[[nodiscard]] constexpr static size_t align(size_t value, size_t alignment) noexcept
		{
			return (value + alignment - 1) & ~(alignment - 1);
		}

	private: // MEMBERS
		inline static size_t reservedBytes;
		inline static size_t skipReservedBytes;

		size_t pageCount;
		size_t highWaterMark = 0;
		size_t elementCount = 0;
		Node* data;
		Node* endData;
		uint32_t* skipArray;
		FreeListNode* freeList = nullptr;
	};


	class OS_PAGE_INFO
	{
		template<class, size_t, Use_Generations_Concept>
		friend class stableVector;


		inline static size_t pageSize;


		static void getSystemPageData()
		{
			[[maybe_unused]] static const bool _ = []() noexcept -> bool
				{
					SYSTEM_INFO systemInfo;
					GetSystemInfo(&systemInfo);
					pageSize = static_cast<size_t>(systemInfo.dwPageSize);

					return false;
				}();
		}
	};

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

	// Simplified, never needs to grow or make checks
	template<class KeyValue>
	class RemapMap
	{
	private:
		struct RemapNode
		{
			int64_t psl = -1;
			KeyValue key = KeyValue{};
			KeyValue value = KeyValue{};

			[[nodiscard]] bool isEmpty() const { return psl == -1; };
		};

	private:
		RemapMap() noexcept = default;

	public:
		RemapMap(const RemapMap& other)
		{
			size = other.size;

			data = static_cast<RemapNode*>(::operator new(other.size * sizeof(RemapNode), std::alignment_of<RemapNode>::value));
			memcpy(data, other.data, size * sizeof(RemapNode));
		}


		RemapMap& operator=(const RemapMap& other)
		{
			if (this != &other)
			{
				::operator delete(data, size * sizeof(RemapNode), std::alignment_of<RemapNode>::value);

				size = other.size;

				data = static_cast<RemapNode*>(::operator new(other.size * sizeof(RemapNode), std::alignment_of<RemapNode>::value));
				memcpy(data, other.data, size * sizeof(RemapNode));
			}

			return *this;
		}


		RemapMap(RemapMap&& other) noexcept
		{
			data = other.data;
			size = other.size;

			other.data = nullptr;
			other.size = 0;
		}


		RemapMap& operator=(RemapMap&& other) noexcept
		{
			if (this != &other)
			{
				::operator delete(data, size * sizeof(RemapNode), std::align_val_t(std::alignment_of<RemapNode>::value));

				data = other.data;
				size = other.size;

				other.data = nullptr;
				other.size = 0;
			}

			return *this;
		}


		~RemapMap() noexcept
		{
			::operator delete(data, size * sizeof(RemapNode), std::align_val_t(std::alignment_of<RemapNode>::value));
		}

	private:
		void allocate(size_t elementCount)
		{
			size = elementCount;

			data = static_cast<RemapNode*>(::operator new(size * sizeof(RemapNode), std::align_val_t(std::alignment_of<RemapNode>::value)));

			for (size_t index = 0; index != size; ++index)
			{
				data[index] = RemapNode{ -1, KeyValue{}, KeyValue{} };
			}
		}


		void insert(KeyValue key, KeyValue value)
		{
			size_t index = std::hash<KeyValue>()(key) & (size - 1);
			int64_t psl = 0;

			while (true)
			{
				if (data[index].isEmpty())
				{
					data[index].psl = psl;
					data[index].key = key;
					data[index].value = value;

					return;
				}
				else if (psl > data[index].psl)
				{
					std::swap(psl, data[index].psl);
					std::swap(key, data[index].key);
					std::swap(value, data[index].value);
				}

				++psl;
				++index;

				if (index == size)
				{
					index = 0;
				}
			}
		}

	public:
		[[nodiscard]] KeyValue find(KeyValue key) const
		{
			size_t start = std::hash<KeyValue>()(key) & (size - 1);
			size_t index = start;
			int64_t psl = 0;

			while (key != data[index].key)
			{
				if (psl > data[index].psl)
				{
					return KeyValue{};
				}

				++psl;
				++index;

				if (index == size)
				{
					index = 0;
				}
				if (index == start)
				{
					return KeyValue{};
				}
			}

			return data[index].value;
		}


		[[nodiscard]] bool isEmpty() const noexcept
		{
			return !size;
		}

	private:
		template<class, size_t, Use_Generations_Concept>
		friend class stableVector;


		RemapNode* data = nullptr;
		size_t size = 0;
	};
}
