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
	struct return_map {};
	struct no_map {};


	template<class T>
	concept ConstIteratorConcept = std::same_as<T, is_const> || std::same_as<T, not_const>;

	template<class T>
	concept UseGenerationsConcept = std::same_as<T, use_generations> || std::same_as<T, no_generations>;

	template<class T>
	concept ReturnRemapMapConcept = std::same_as<T, return_map> || std::same_as<T, no_map>;

	template<class, size_t, UseGenerationsConcept, ConstIteratorConcept>
	class stableVectorIterator;

	template<class>
	class RemapMap;

	class OS_PAGE_INFO;



	// STABLE VECTOR
	template<class T, size_t t_VMReserveElements, UseGenerationsConcept t_useGenerations = no_generations>
		requires (t_VMReserveElements <= UINT32_MAX)
	class stableVector
	{
	private:
		static_assert(std::is_nothrow_destructible_v<T>, "gbr::stableVector requires T to be nothrow destructible");

	private: // TYPES
		struct FreeListIndices
		{
			uint32_t next;
			uint32_t previous;
		};


		struct IndividualisticNode
		{
			union
			{
				T value;
				FreeListIndices indices;
			};
		};


		struct GenerationalNode
		{
			uint32_t generation;

			union
			{
				T value;
				FreeListIndices indices;
			};
		};

	public:
		struct IndividualisticHandle
		{
			uint32_t index;
		};


		struct GenerationalHandle
		{
			uint32_t index;
			uint32_t generation;
		};

	private: // MEMBER ALIASES
		template<class, size_t, UseGenerationsConcept, ConstIteratorConcept>
		friend class stableVectorIterator;


		constexpr static bool c_generational = std::same_as<t_useGenerations, use_generations>;


		using Node = std::conditional_t<c_generational, GenerationalNode, IndividualisticNode>;

	public:
		using Handle = std::conditional_t<c_generational, GenerationalHandle, IndividualisticHandle>;
		using Iterator = stableVectorIterator<T, t_VMReserveElements, t_useGenerations, not_const>;
		using ConstIterator = stableVectorIterator<T, t_VMReserveElements, t_useGenerations, is_const>;

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

				m_highWaterMark = 0;

				copyStableVector(other);
			}

			return *this;
		}


		stableVector(stableVector&& other) noexcept
		{
			m_data = other.m_data;
			m_endData = other.m_endData;
			m_skipData = other.m_skipData;
			m_pageCount = other.m_pageCount;
			m_highWaterMark = other.m_highWaterMark;
			m_size = other.m_size;

			other.m_data = nullptr;
			other.m_endData = nullptr;
			other.m_skipData = nullptr;
			other.m_pageCount = 0;
			other.m_highWaterMark = 0;
			other.m_size = 0;
		}


		stableVector& operator=(stableVector&& other) noexcept
		{
			if (this != &other)
			{
				deallocate();

				m_data = other.m_data;
				m_endData = other.m_endData;
				m_skipData = other.m_skipData;
				m_pageCount = other.m_pageCount;
				m_highWaterMark = other.m_highWaterMark;
				m_size = other.m_size;

				other.m_data = nullptr;
				other.m_endData = nullptr;
				other.m_skipData = nullptr;
				other.m_pageCount = 0;
				other.m_highWaterMark = 0;
				other.m_size = 0;
			}

			return *this;
		}


		~stableVector() noexcept
		{
			deallocate();
		}

	public: // MEMBER FUNCTIONS
		Handle insert(const T& value)
		{
			return emplace(value);
		}


		Handle insert(T&& value)
		{
			return emplace(std::move(value));
		}


		template<class... Args>
		Handle emplace(Args&&... args)
		{
			Node* const slot = getAllocationSlot();

			::new(&slot->value) T(std::forward<Args>(args)...);

			++m_size;

			if constexpr (c_generational)
			{
				++(slot->generation);

				return { static_cast<uint32_t>(slot - m_data - 1), slot->generation };
			}
			else
			{
				return { static_cast<uint32_t>(slot - m_data - 1) };
			}
		}


		void erase(uint32_t index) noexcept
		{
			m_data[index + 1].value.~T();

			updateSkipArray(index + 1);

			if constexpr (c_generational)
			{
				++m_data[index + 1].generation;
			}

			--m_size;
		}


		void erase(Handle handle) noexcept
		{
			erase(handle.index);
		}


		Iterator erase(const Iterator& iterator) noexcept
		{
			Iterator nextElement = iterator;
			++nextElement;

			iterator.data->value.~T();

			updateSkipArray(iterator.skipPtr - m_skipData);

			if constexpr (c_generational)
			{
				++(iterator.data->generation);
			}

			--m_size;

			return nextElement;
		}

		// HELPERS
		[[nodiscard]] T& at(uint32_t index) noexcept
		{
			return m_data[index + 1].value;
		}


		[[nodiscard]] const T& at(const uint32_t index) const noexcept
		{
			return m_data[index + 1].value;
		}


		[[nodiscard]] T& at(Handle handle) noexcept
		{
			return at(handle.index);
		}


		[[nodiscard]] const T& at(const Handle handle) const noexcept
		{
			return at(handle.index);
		}


		[[nodiscard]] size_t size() const noexcept
		{
			return m_size;
		}


		[[nodiscard]] bool isAlive(uint32_t index) const noexcept
		{
			return !m_skipData[index + 1];
		}


		[[nodiscard]] bool isAlive(Handle handle) const noexcept
		{
			return isAlive(handle.index);
		}


		[[nodiscard]] bool isValid(Handle handle) const noexcept
			requires c_generational
		{
			return m_data[handle.index + 1].generation == handle.generation;
		}


		[[nodiscard]] uint32_t& getGeneration(uint32_t index) noexcept
			requires c_generational
		{
			return m_data[index + 1].generation;
		}


		[[nodiscard]] const uint32_t& getGeneration(const uint32_t index) const noexcept
			requires c_generational
		{
			return m_data[index + 1].generation;
		}


		[[nodiscard]] uint32_t& getGeneration(Handle handle) noexcept
			requires c_generational
		{
			return getGeneration(handle.index);
		}


		[[nodiscard]] const uint32_t& getGeneration(const Handle handle) const noexcept
			requires c_generational
		{
			return getGeneration(handle.index);
		}


		void reserve(uint32_t reserveElements)
		{
			if (const uint32_t newPageCount = align(reserveElements * sizeof(Node), OS_PAGE_INFO::pageSize) / OS_PAGE_INFO::pageSize; newPageCount > m_pageCount)
			{
				grow(newPageCount);
			}
		}

		// Intended to break pointer stability
		template<ReturnRemapMapConcept t_returnMap = return_map, class Allocator = std::allocator<uint32_t>>
		std::conditional_t<std::same_as<t_returnMap, return_map>, RemapMap<Allocator>, no_map> compress()
			requires std::is_nothrow_move_constructible_v<T>
		{
			static constexpr bool c_returnMap = std::same_as<t_returnMap, return_map>;
			std::conditional_t<c_returnMap, RemapMap<Allocator>, no_map> remapMap;

			uint32_t lastIndex = 0;

			if (m_size)
			{
				lastIndex = cback().data - m_data;
				size_t elementsToMove = 0;

				if constexpr (c_returnMap)
				{
					size_t skipSlots = 0;
					size_t countedElements = 0;

					for (const uint32_t* currentIndex = m_skipData; skipSlots < m_size - countedElements;)
					{
						++currentIndex;
						++countedElements;
						skipSlots += *currentIndex;
						currentIndex += *currentIndex;
					}

					elementsToMove = m_size - countedElements;
				}
				else
				{
					elementsToMove = lastIndex - m_size;
				}

				if (elementsToMove)
				{
					if constexpr (c_returnMap)
					{
						size_t pow2ElementsToMove = std::bit_ceil(elementsToMove);

						if (static_cast<float>(elementsToMove) / static_cast<float>(pow2ElementsToMove) > 0.6f)
						{
							pow2ElementsToMove *= 2;
						}

						remapMap.allocate(pow2ElementsToMove);
					}

					for (uint32_t currentIndex = 1; currentIndex < lastIndex; ++currentIndex)
					{
						if (m_skipData[currentIndex])
						{
							::new(&m_data[currentIndex].value) T(std::move(m_data[lastIndex].value));
							m_data[lastIndex].value.~T();

							if constexpr (c_generational)
							{
								m_data[currentIndex].generation = m_data[lastIndex].generation;
							}

							if constexpr (c_returnMap)
							{
								remapMap.insert(lastIndex - 1, currentIndex - 1);
							}

							while (m_skipData[--lastIndex] && lastIndex > currentIndex) {}
						}
					}
				}

				decommitPages(m_size);
			}
			else
			{
				decommitPages(lastIndex);
			}

			m_highWaterMark = m_size;
			m_data[0].indices = FreeListIndices{};

			if constexpr (c_generational)
			{
				for (uint32_t currentIndex = m_highWaterMark + 1; m_data + currentIndex != m_endData; ++currentIndex)
				{
					::new(m_data + currentIndex) Node{};
				}
			}

			memset(m_skipData, 0, getSkipArrayBytes());

			return remapMap;
		}


		void shrinkToFit() noexcept
		{
			uint32_t index = 0;

			m_data[0].indices = FreeListIndices{};

			if (m_size)
			{
				index = cback().data - m_data;

				decommitPages(index);

				if constexpr (c_generational)
				{
					memset(m_data + index + 1, 0, (m_endData - m_data - index - 1) * sizeof(Node));
				}

				memset(m_skipData + index + 1, 0, getSkipArrayBytes() - (index + 1) * sizeof(uint32_t));

				uint32_t newFreeListIndex = 0;
				for (const uint32_t* currentIndex = m_skipData + index; currentIndex != m_skipData;)
				{
					--currentIndex;
					currentIndex -= *currentIndex;

					const uint32_t freeListIndex = currentIndex - m_skipData;

					if (m_skipData[freeListIndex + 1])
					{
						m_data[freeListIndex + 1].indices.next = newFreeListIndex;
						m_data[newFreeListIndex].indices.previous = freeListIndex + 1;
						newFreeListIndex = freeListIndex + 1;
					}
				}

				m_data[newFreeListIndex].indices.previous = 0;
				m_data[0].indices.next = newFreeListIndex;

				m_highWaterMark = index;

				return;
			}

			decommitPages(index);

			m_highWaterMark = 0;
			m_data[0].indices = FreeListIndices{};

			memset(m_skipData, 0, getSkipArrayBytes());
		}

		// ITERATORS
		[[nodiscard]] Iterator begin() noexcept
		{
			return Iterator(m_data + m_skipData[1] + 1, m_skipData + m_skipData[1] + 1);
		}


		[[nodiscard]] Iterator end() noexcept
		{
			return Iterator(m_data + m_highWaterMark + 1, m_skipData + m_highWaterMark + 1);
		}


		[[nodiscard]] Iterator back() noexcept
		{
			Iterator toReturn(m_data + m_highWaterMark + 1, m_skipData + m_highWaterMark + 1);

			return m_size ? --toReturn : toReturn;
		}


		[[nodiscard]] ConstIterator begin() const noexcept
		{
			return ConstIterator(m_data + m_skipData[1] + 1, m_skipData + m_skipData[1] + 1);
		}


		[[nodiscard]] ConstIterator end() const noexcept
		{
			return ConstIterator(m_data + m_highWaterMark + 1, m_skipData + m_highWaterMark + 1);
		}


		[[nodiscard]] ConstIterator back() const noexcept
		{
			ConstIterator toReturn(m_data + m_highWaterMark + 1, m_skipData + m_highWaterMark + 1);

			return m_size ? --toReturn : toReturn;
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
		void allocate(uint32_t reserveElements)
		{
			[[maybe_unused]] static const bool _ = []() noexcept -> bool
				{
					OS_PAGE_INFO::getSystemPageData();

					sm_reservedBytes = (t_VMReserveElements + 1) * sizeof(Node);
					sm_reservedBytes = align(sm_reservedBytes, OS_PAGE_INFO::pageSize);
					sm_skipReservedBytes = (sm_reservedBytes / sizeof(Node) + 1) * sizeof(uint32_t);
					sm_skipReservedBytes = align(sm_skipReservedBytes, OS_PAGE_INFO::pageSize);

					return false;
				}();

			const size_t elementsToReserve = std::clamp(static_cast<size_t>(reserveElements), 1ULL, t_VMReserveElements + 1);

			size_t reserveSize = (elementsToReserve + 1) * sizeof(Node);
			reserveSize = align(reserveSize, OS_PAGE_INFO::pageSize);
			size_t skipReserveSize = (reserveSize / sizeof(Node) + 1) * sizeof(uint32_t);
			skipReserveSize = align(skipReserveSize, OS_PAGE_INFO::pageSize);

			m_pageCount = reserveSize / OS_PAGE_INFO::pageSize;

			m_data = static_cast<Node*>(VirtualAlloc(NULL, sm_reservedBytes, MEM_RESERVE, PAGE_READWRITE));
			m_skipData = static_cast<uint32_t*>(VirtualAlloc(NULL, sm_skipReservedBytes, MEM_RESERVE, PAGE_READWRITE));

			if (!m_data || !m_skipData)
			{
				goto FAIL;
			}
			if (!VirtualAlloc(m_data, reserveSize, MEM_COMMIT, PAGE_READWRITE) ||
				!VirtualAlloc(m_skipData, skipReserveSize, MEM_COMMIT, PAGE_READWRITE))
			{
				goto FAIL;
			}

			m_endData = m_data + reserveSize / sizeof(Node);

			::new(&m_data[0].indices) FreeListIndices{};

			return;

		FAIL:
			if (m_data)
			{
				if (!VirtualFree(m_data, 0, MEM_RELEASE))
				{
					goto FAIL1;
				}
			}
			if (m_skipData)
			{
				if (!VirtualFree(m_skipData, 0, MEM_RELEASE))
				{
					goto FAIL1;
				}
			}

			throw std::bad_alloc();

		FAIL1:
			std::cerr << "VirtualFree failed\n";
			std::terminate();
		}


		void copyStableVector(const stableVector& other)
		{
			m_size = other.m_size;
			m_highWaterMark = other.m_highWaterMark;

			allocate(m_highWaterMark);

			memcpy(m_skipData, other.m_skipData, (m_highWaterMark + 1) * sizeof(uint32_t));

			if constexpr (std::is_trivially_copy_constructible_v<T>)
			{
				memcpy(m_data, other.m_data, (m_highWaterMark + 1) * sizeof(Node));
			}
			else
			{
				size_t currentIndex = 0;

				try
				{
					for (; currentIndex != m_highWaterMark + 1; ++currentIndex)
					{
						if (m_skipData[currentIndex])
						{
							::new(&m_data[currentIndex].indices) FreeListIndices(other.m_data[currentIndex].indices);
						}
						else
						{
							::new(&m_data[currentIndex].value) T(other.m_data[currentIndex].value);
						}

						if constexpr (c_generational)
						{
							m_data[currentIndex].generation = other.m_data[currentIndex].generation;
						}
					}
				}
				catch (...)
				{
					deallocate<true>(currentIndex - 1);

					throw;
				}
			}
		}


		template<bool t_enableLastIndex = false>
		void deallocate(uint32_t lastIndex = 0) noexcept
		{
			if (m_data)
			{
				for (T& element : *this)
				{
					element.~T();

					if constexpr (t_enableLastIndex)
					{
						if (reinterpret_cast<Node*>(reinterpret_cast<char*>(element) - offsetof(Node, value)) - m_data >= lastIndex)
						{
							break;
						}
					}
				}

				if (!VirtualFree(m_data, 0, MEM_RELEASE) ||
					!VirtualFree(m_skipData, 0, MEM_RELEASE))
				{
					std::cerr << "VirtualFree failed\n";
					std::terminate();
				}
			}
		}


		[[nodiscard]] size_t getSkipArrayBytes() const noexcept
		{
			const size_t skipArrayBytes = (m_pageCount * OS_PAGE_INFO::pageSize / sizeof(Node) + 1) * sizeof(uint32_t);

			return align(skipArrayBytes, OS_PAGE_INFO::pageSize);
		}


		void decommitPages(uint32_t index) noexcept
		{
			size_t bytesUsed = (index + 1) * sizeof(Node);
			bytesUsed = align(bytesUsed, OS_PAGE_INFO::pageSize);
			const uint32_t pagesUsed = bytesUsed / OS_PAGE_INFO::pageSize;
			const size_t bytesToDecommit = (m_pageCount - pagesUsed) * OS_PAGE_INFO::pageSize;

			const size_t prevSkipArrayBytes = getSkipArrayBytes();

			m_pageCount = pagesUsed;

			const size_t skipArrayBytes = getSkipArrayBytes();
			const size_t skipArrayBytesToDecommit = (prevSkipArrayBytes - skipArrayBytes);

			if (bytesToDecommit)
			{
				sm_isFull = false;

				if (!VirtualFree(reinterpret_cast<char*>(m_data) + bytesUsed, bytesToDecommit, MEM_DECOMMIT))
				{
					goto FAIL;
				}
			}
			if (skipArrayBytesToDecommit)
			{
				if (!VirtualFree(reinterpret_cast<char*>(m_skipData) + skipArrayBytes, skipArrayBytesToDecommit, MEM_DECOMMIT))
				{
					goto FAIL;
				}
			}

			m_endData = m_data + bytesUsed / sizeof(Node);

			return;

		FAIL:
			std::cerr << "VirtualFree failed\n";
			std::terminate();
		}


		[[nodiscard]] Node* getAllocationSlot()
		{
			FreeListIndices& freeList = getFreeListHead();

			if (freeList.next)
			{
				const uint32_t currentIndex = freeList.next;
				const uint32_t jumpSize = m_skipData[currentIndex];

				if (jumpSize != 1)
				{
					++freeList.next;

					::new(&m_data[currentIndex + 1].indices) FreeListIndices(m_data[currentIndex].indices);
					m_data[m_data[currentIndex].indices.next].indices.previous = currentIndex + 1;

					m_skipData[currentIndex] = 0;
					m_skipData[currentIndex + 1] = jumpSize - 1;
					m_skipData[currentIndex + jumpSize - 1] = jumpSize - 1;
				}
				else
				{
					m_skipData[currentIndex] = 0;

					const uint32_t nextFreeListIndex = m_data[currentIndex].indices.next;

					freeList.next = nextFreeListIndex;
					m_data[nextFreeListIndex].indices.previous = 0;
				}

				return m_data + currentIndex;
			}
			else if (m_data + m_highWaterMark + 1 == m_endData)
			{
				grow(m_pageCount * 2);
			}

			++m_highWaterMark;

			return m_data + m_highWaterMark;
		}


		void grow(uint32_t newPageCount)
		{
			if (!sm_isFull)
			{
				const uint32_t maxPageCount = static_cast<uint32_t>(sm_reservedBytes / OS_PAGE_INFO::pageSize);

				if (newPageCount >= maxPageCount)
				{
					sm_isFull = true;
				}

				m_pageCount = std::min(newPageCount, maxPageCount);
				m_endData = m_data + m_pageCount * OS_PAGE_INFO::pageSize / sizeof(Node);

				const bool dataCode = VirtualAlloc(m_data, (m_pageCount * OS_PAGE_INFO::pageSize), MEM_COMMIT, PAGE_READWRITE);
				const bool skipDataCode = VirtualAlloc(m_skipData, getSkipArrayBytes(), MEM_COMMIT, PAGE_READWRITE);

				if (!dataCode || !skipDataCode)
				{
					goto FAIL;
				}
			}

			return;

		FAIL:
			deallocate();

			throw std::bad_alloc();
		}


		void updateSkipArray(uint32_t currentSkipNodeIndex) noexcept
		{
			const uint32_t leftSkipSize = *(m_skipData + currentSkipNodeIndex - 1);
			uint32_t* const currentSkipNode = m_skipData + currentSkipNodeIndex;
			const uint32_t rightSkipSize = m_skipData[currentSkipNodeIndex + 1];

			if (leftSkipSize)
			{
				if (rightSkipSize)
				{
					const uint32_t totalSkipSize = leftSkipSize + rightSkipSize + 1;

					*(currentSkipNode - leftSkipSize) = totalSkipSize;
					*currentSkipNode = totalSkipSize;
					currentSkipNode[rightSkipSize] = totalSkipSize;

					const FreeListIndices indices = m_data[currentSkipNodeIndex + 1].indices;

					m_data[indices.previous].indices.next = indices.next;
					m_data[indices.next].indices.previous = indices.previous;

					//::new(&m_data[currentSkipNodeIndex].indices) FreeListIndices{};
					//::new(&m_data[currentSkipNodeIndex + 1].indices) FreeListIndices{};

					return;
				}

				*(currentSkipNode - leftSkipSize) = leftSkipSize + 1;
				*currentSkipNode = leftSkipSize + 1;

				//::new(&m_data[currentSkipNodeIndex].indices) FreeListIndices{};
			}
			else if (rightSkipSize)
			{
				*currentSkipNode = rightSkipSize + 1;
				currentSkipNode[rightSkipSize] = rightSkipSize + 1;

				const FreeListIndices indices = m_data[currentSkipNodeIndex + 1].indices;

				::new(&m_data[currentSkipNodeIndex].indices) FreeListIndices(indices);

				m_data[indices.previous].indices.next = currentSkipNodeIndex;
				m_data[indices.next].indices.previous = currentSkipNodeIndex;
			}
			else
			{
				*currentSkipNode = 1;

				FreeListIndices& freeList = getFreeListHead();

				::new(&m_data[currentSkipNodeIndex].indices) FreeListIndices{ freeList.next, 0 };

				m_data[freeList.next].indices.previous = currentSkipNodeIndex;

				freeList.next = currentSkipNodeIndex;
			}
		}


		[[nodiscard]] constexpr static size_t align(size_t value, size_t alignment) noexcept
		{
			return (value + alignment - 1) & ~(alignment - 1);
		}


		[[nodiscard]] FreeListIndices& getFreeListHead()
		{
			return m_data[0].indices;
		}

	private: // MEMBERS
		inline static bool sm_isFull = false;
		inline static size_t sm_reservedBytes;
		inline static size_t sm_skipReservedBytes;

		Node* m_data = nullptr;
		Node* m_endData = nullptr;
		uint32_t* m_skipData = nullptr;
		uint32_t m_pageCount;
		uint32_t m_highWaterMark = 0;
		uint32_t m_size = 0;
	};



	// ITERATOR
	template<class T, size_t elements, UseGenerationsConcept UseGenerations, ConstIteratorConcept IsConst>
	class stableVectorIterator
	{
	private:
		template<class, size_t t_VMReserveElements, UseGenerationsConcept>
			requires (t_VMReserveElements <= UINT32_MAX)
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
			return reinterpret_cast<ValueType*>(&data->value);
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



	// REMAP MAP, simplified, never needs to grow or make checks
	template<class Allocator>
	class RemapMap
	{
	private:
		struct RemapNode
		{
			int32_t psl = -1;
			uint32_t key = 0;
			uint32_t value = 0;

			[[nodiscard]] bool isEmpty() const { return psl == -1; };
		};

	private:
		using NodeAllocator = typename std::allocator_traits<Allocator>::template rebind_alloc<RemapNode>;

	private:
		RemapMap() noexcept = default;

		RemapMap(const Allocator& alloc) noexcept : m_state(alloc) {}

	public:
		RemapMap(const RemapMap& other) : m_state(other.m_state)
		{
			m_state.data = m_state.allocate(m_state.size);
			memcpy(m_state.data, other.m_state.data, m_state.size * sizeof(RemapNode));
		}


		RemapMap& operator=(const RemapMap& other)
		{
			if (this != &other)
			{
				CompressedState newState = other.m_state;
				newState.data = newState.allocate(newState.size);

				m_state.deallocate(m_state.data, m_state.size);
				memcpy(newState.data, other.m_state.data, newState.size * sizeof(RemapNode));

				m_state = std::move(newState);
			}

			return *this;
		}


		RemapMap(RemapMap&& other) noexcept : m_state(std::move(other.m_state))
		{
			other.m_state.data = nullptr;
			other.m_state.size = 0;
		}


		RemapMap& operator=(RemapMap&& other) noexcept
		{
			if (this != &other)
			{
				m_state.deallocate(m_state.data, m_state.size);

				m_state = std::move(other.m_state);

				other.m_state.data = nullptr;
				other.m_state.size = 0;
			}

			return *this;
		}


		~RemapMap() noexcept
		{
			m_state.deallocate(m_state.data, m_state.size);
		}

	private:
		void allocate(size_t elementCount)
		{
			m_state.size = elementCount;

			m_state.data = m_state.allocate(m_state.size);

			for (size_t index = 0; index != m_state.size; ++index)
			{
				m_state.data[index] = RemapNode();
			}
		}


		void insert(uint32_t key, uint32_t value) noexcept
		{
			size_t index = std::hash<uint32_t>()(key) & (m_state.size - 1);
			int32_t psl = 0;

			while (true)
			{
				if (m_state.data[index].isEmpty())
				{
					m_state.data[index].psl = psl;
					m_state.data[index].key = key;
					m_state.data[index].value = value;

					return;
				}
				else if (psl > m_state.data[index].psl)
				{
					std::swap(psl, m_state.data[index].psl);
					std::swap(key, m_state.data[index].key);
					std::swap(value, m_state.data[index].value);
				}

				++psl;
				++index;

				if (index == m_state.size)
				{
					index = 0;
				}
			}
		}

	public:
		[[nodiscard]] uint32_t find(uint32_t key) const noexcept
		{
			const size_t start = std::hash<uint32_t>()(key) & (m_state.size - 1);
			size_t index = start;
			int32_t psl = 0;

			while (key != m_state.data[index].key)
			{
				if (psl > m_state.data[index].psl)
				{
					return key;
				}

				++psl;
				++index;

				if (index == m_state.size)
				{
					index = 0;
				}
				if (index == start)
				{
					return key;
				}
			}

			return m_state.data[index].value;
		}


		[[nodiscard]] bool isEmpty() const noexcept
		{
			return !m_state.size;
		}

	private:
		template<class, size_t t_VMReserveElements, UseGenerationsConcept>
			requires (t_VMReserveElements <= UINT32_MAX)
		friend class stableVector;


		struct CompressedState : public NodeAllocator
		{
			RemapNode* data = nullptr;
			size_t size = 0;
		};

		CompressedState m_state;
	};


	// OS PAGE INFO
	class OS_PAGE_INFO
	{
		template<class, size_t t_VMReserveElements, UseGenerationsConcept>
			requires (t_VMReserveElements <= UINT32_MAX)
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
}
