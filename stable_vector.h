#pragma once

#include <type_traits>
#include <algorithm>
#include <concepts>
#include <cstdint>
#include <ranges>
#include <bit>
#include <new>

#define NOMINMAX
#include <Windows.h>


namespace gbr
{
	// CONCEPTS
	struct use_generations {};
	struct no_generations {};
	struct is_const {};
	struct not_const {};
	struct branchless {};
	struct branched {};
	struct return_map {};
	struct no_map {};


	template<class T>
	concept const_iterator_concept = std::same_as<T, is_const> || std::same_as<T, not_const>;

	template<class T>
	concept use_generations_concept = std::same_as<T, use_generations> || std::same_as<T, no_generations>;

	template<class T>
	concept return_remap_map_concept = std::same_as<T, return_map> || std::same_as<T, no_map>;

	template<class T>
	concept iterator_branch_concept = std::same_as<T, branchless> || std::same_as<T, branched>;


	template<class, size_t, use_generations_concept, const_iterator_concept, iterator_branch_concept t_branched = branchless>
	class stable_vector_iterator;

	template<class>
	class remap_map;


	namespace implementation
	{
		template<class, size_t, use_generations_concept, const_iterator_concept, iterator_branch_concept>
		class stable_vector_iterator_base;


		inline size_t OS_PAGE_SIZE;


		inline void get_system_page_data()
		{
			[[maybe_unused]] static const bool _ = []() noexcept -> bool
			{
				SYSTEM_INFO system_info;
				GetSystemInfo(&system_info);
				OS_PAGE_SIZE = static_cast<size_t>(system_info.dwPageSize);

				return false;
			}();
		}


		[[nodiscard]] constexpr inline size_t align(size_t p_value, size_t p_alignment) noexcept
		{
			return (p_value + p_alignment - 1) & ~(p_alignment - 1);
		}
	}



	// STABLE VECTOR
	template<class T, size_t t_VM_reserve_elements, use_generations_concept t_use_generations = no_generations>
		requires (t_VM_reserve_elements <= UINT32_MAX)
	class stable_vector
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
		template<class, size_t, use_generations_concept, const_iterator_concept, iterator_branch_concept>
		friend class implementation::stable_vector_iterator_base;


		constexpr inline static bool c_generational = std::same_as<t_use_generations, use_generations>;


		using Node = std::conditional_t<c_generational, GenerationalNode, IndividualisticNode>;

	public:
		using handle = std::conditional_t<c_generational, GenerationalHandle, IndividualisticHandle>;
		using iterator = stable_vector_iterator<T, t_VM_reserve_elements, t_use_generations, not_const, branchless>;
		using const_iterator = stable_vector_iterator<T, t_VM_reserve_elements, t_use_generations, is_const, branchless>;
		using branched_iterator = stable_vector_iterator<T, t_VM_reserve_elements, t_use_generations, not_const, branched>;
		using const_branched_iterator = stable_vector_iterator<T, t_VM_reserve_elements, t_use_generations, is_const, branched>;

	public: // CONSTRUCTORS
		stable_vector()
		{
			allocate_(1);
		}


		explicit stable_vector(uint32_t p_reserve_count)
		{
			allocate_(p_reserve_count);
		}


		stable_vector(uint32_t p_element_count, const T& p_value)
		{
			allocate_(p_element_count);

			for (uint32_t index = 0; index < p_element_count; ++index)
			{
				emplace_back_unchecked(p_value);
			}
		}


		template <std::ranges::input_range t_range>
		explicit stable_vector(t_range&& p_range)
		{
			if constexpr (std::ranges::sized_range<std::remove_reference_t<t_range>>)
			{
				allocate_(std::ranges::size(p_range));

				for (auto&& element : p_range)
				{
					emplace_back_unchecked(element);
				}
			}
			else
			{
				allocate_(1);

				for (auto&& element : p_range)
				{
					emplace_back(element);
				}
			}
		}


		template <class t_iterator>
		stable_vector(t_iterator p_first, t_iterator p_last)
		{
			if constexpr (std::random_access_iterator<t_iterator>)
			{
				const uint32_t elementCount = static_cast<uint32_t>(p_last - p_first);
				allocate_(elementCount);

				for (; p_first != p_last; ++p_first)
				{
					emplace_back_unchecked(*p_first);
				}
			}
			else
			{
				allocate_(1);

				for (; p_first != p_last; ++p_first)
				{
					emplace_back(*p_first);
				}
			}
		}


		stable_vector(const stable_vector& p_other)
		{
			copy_stable_vector_(p_other);
		}


		stable_vector& operator=(const stable_vector& p_other)
		{
			if (this != &p_other)
			{
				stable_vector temp(p_other);
				std::swap(*this, temp);
			}

			return *this;
		}


		stable_vector(stable_vector&& p_other) noexcept
		{
			m_data = p_other.m_data;
			m_end_data = p_other.m_end_data;
			m_skip_data = p_other.m_skip_data;
			m_page_count = p_other.m_page_count;
			m_high_water_mark = p_other.m_high_water_mark;
			m_size = p_other.m_size;

			p_other.m_data = nullptr;
			p_other.m_end_data = nullptr;
			p_other.m_skip_data = nullptr;
			p_other.m_page_count = 0;
			p_other.m_high_water_mark = 0;
			p_other.m_size = 0;
		}


		stable_vector& operator=(stable_vector&& p_other) noexcept
		{
			if (this != &p_other)
			{
				deallocate_();

				m_data = p_other.m_data;
				m_end_data = p_other.m_end_data;
				m_skip_data = p_other.m_skip_data;
				m_page_count = p_other.m_page_count;
				m_high_water_mark = p_other.m_high_water_mark;
				m_size = p_other.m_size;

				p_other.m_data = nullptr;
				p_other.m_end_data = nullptr;
				p_other.m_skip_data = nullptr;
				p_other.m_page_count = 0;
				p_other.m_high_water_mark = 0;
				p_other.m_size = 0;
			}

			return *this;
		}


		~stable_vector() noexcept
		{
			deallocate_();
		}

	public: // MEMBER FUNCTIONS
		handle insert(const T& p_value)
		{
			return emplace(p_value);
		}


		handle insert(T&& p_value)
		{
			return emplace(std::move(p_value));
		}


		template<class... Args>
		handle emplace(Args&&... p_args)
		{
			Node* const slot = get_allocation_slot_();

			::new(&slot->value) T(std::forward<Args>(p_args)...);

			++m_size;

			if constexpr (c_generational)
			{
				return { static_cast<uint32_t>(slot - m_data - 1), slot->generation };
			}
			else
			{
				return { static_cast<uint32_t>(slot - m_data - 1) };
			}
		}


		handle push_back(const T& p_value)
		{
			return emplace_back(p_value);
		}


		handle push_back(T&& p_value)
		{
			return emplace_back(std::move(p_value));
		}


		template<class... Args>
		handle emplace_back(Args&&... p_args)
		{
			Node* const slot = get_end_allocation_slot_();

			::new(&slot->value) T(std::forward<Args>(p_args)...);

			++m_size;

			if constexpr (c_generational)
			{
				return { static_cast<uint32_t>(slot - m_data - 1), slot->generation };
			}
			else
			{
				return { static_cast<uint32_t>(slot - m_data - 1) };
			}
		}


		handle push_back_unchecked(const T& p_value)
		{
			return emplace_back_unchecked(p_value);
		}


		handle push_back_unchecked(T&& p_value)
		{
			return emplace_back_unchecked(std::move(p_value));
		}


		template<class... Args>
		handle emplace_back_unchecked(Args&&... p_args)
		{
			Node* const slot = get_unchecked_allocation_slot_();

			::new(&slot->value) T(std::forward<Args>(p_args)...);

			++m_size;

			if constexpr (c_generational)
			{
				return { static_cast<uint32_t>(slot - m_data - 1), slot->generation };
			}
			else
			{
				return { static_cast<uint32_t>(slot - m_data - 1) };
			}
		}


		void erase(uint32_t p_index) noexcept
		{
			m_data[p_index + 1].value.~T();

			update_skip_array_(p_index + 1);

			if constexpr (c_generational)
			{
				++m_data[p_index + 1].generation;
			}

			--m_size;
		}


		void erase(handle p_handle) noexcept
		{
			erase(p_handle.index);
		}


		void erase(T* p_element) noexcept
		{
			erase(node_of_(p_element) - m_data - 1);
		}


		iterator erase(const iterator& p_iterator) noexcept
		{
			iterator next_element = p_iterator;
			++next_element;

			p_iterator.m_data->value.~T();

			update_skip_array_(static_cast<uint32_t>(p_iterator.m_skip_ptr - m_skip_data));

			if constexpr (c_generational)
			{
				++(p_iterator.m_data->generation);
			}

			--m_size;

			return next_element;
		}

		// HELPERS
		[[nodiscard]] T& at(uint32_t p_index) noexcept
		{
			return m_data[p_index + 1].value;
		}


		[[nodiscard]] const T& at(uint32_t p_index) const noexcept
		{
			return m_data[p_index + 1].value;
		}


		[[nodiscard]] T& at(handle p_handle) noexcept
		{
			return at(p_handle.index);
		}


		[[nodiscard]] const T& at(handle p_handle) const noexcept
		{
			return at(p_handle.index);
		}


		[[nodiscard]] T& at(T* p_element) noexcept
		{
			return at(node_of_(p_element) - m_data - 1);
		}


		[[nodiscard]] const T& at(const T* p_element) const noexcept
		{
			return at(node_of_(p_element) - m_data - 1);
		}


		[[nodiscard]] bool is_alive(uint32_t p_index) const noexcept
		{
			return !m_skip_data[p_index + 1];
		}


		[[nodiscard]] bool is_alive(handle p_handle) const noexcept
		{
			return is_alive(p_handle.index);
		}


		[[nodiscard]] bool is_alive(T* p_element) const noexcept
		{
			return is_alive(node_of_(p_element) - m_data - 1);
		}


		[[nodiscard]] bool is_alive(const iterator& p_iterator) const noexcept
		{
			return is_alive(p_iterator.m_data - m_data - 1);
		}


		[[nodiscard]] bool is_generation(uint32_t p_index, uint32_t p_generation) const noexcept
			requires c_generational
		{
			return m_data[p_index + 1].generation == p_generation;
		}


		[[nodiscard]] bool is_generation(handle p_handle) const noexcept
			requires c_generational
		{
			return is_generation(p_handle.index, p_handle.generation);
		}


		[[nodiscard]] bool is_generation(T* p_element, uint32_t p_generation) const noexcept
			requires c_generational
		{
			return node_of_(p_element)->generation == p_generation;
		}


		[[nodiscard]] bool is_generation(const iterator& p_iterator, uint32_t p_generation) const noexcept
			requires c_generational
		{
			return p_iterator.m_data->generation == p_generation;
		}


		[[nodiscard]] uint32_t& get_generation(uint32_t p_index) noexcept
			requires c_generational
		{
			return m_data[p_index + 1].generation;
		}


		[[nodiscard]] const uint32_t& get_generation(const uint32_t p_index) const noexcept
			requires c_generational
		{
			return m_data[p_index + 1].generation;
		}


		[[nodiscard]] uint32_t& get_generation(handle p_handle) noexcept
			requires c_generational
		{
			return get_generation(p_handle.index);
		}


		[[nodiscard]] const uint32_t& get_generation(const handle p_handle) const noexcept
			requires c_generational
		{
			return get_generation(p_handle.index);
		}


		[[nodiscard]] uint32_t& get_generation(T* p_element) noexcept
			requires c_generational
		{
			return get_generation(node_of_(p_element) - m_data - 1);
		}


		[[nodiscard]] const uint32_t& get_generation(const T* p_element) const noexcept
			requires c_generational
		{
			return get_generation(node_of_(p_element) - m_data - 1);
		}


		[[nodiscard]] uint32_t& get_generation(const iterator& p_iterator) noexcept
			requires c_generational
		{
			return p_iterator.m_data->generation;
		}


		[[nodiscard]] const uint32_t& get_generation(const iterator& p_iterator) const noexcept
			requires c_generational
		{
			return p_iterator.m_data->generation;
		}


		[[nodiscard]] bool is_empty() const noexcept
		{
			return !m_size;
		}


		[[nodiscard]] uint32_t size() const noexcept
		{
			return m_size;
		}


		[[nodiscard]] uint32_t back_capacity() const noexcept
		{
			return static_cast<uint32_t>(m_end_data - m_data - m_high_water_mark - 1);
		}


		void reserve(uint32_t p_reserve_count)
		{
			const uint32_t new_page_count = implementation::align(p_reserve_count * sizeof(Node), implementation::OS_PAGE_SIZE) / implementation::OS_PAGE_SIZE;

			if (new_page_count > m_page_count)
			{
				grow_(new_page_count);
			}
		}

		// Intended to break pointer stability
		template<return_remap_map_concept t_returnMap = return_map, class Allocator = std::allocator<uint32_t>>
		std::conditional_t<std::same_as<t_returnMap, return_map>, remap_map<Allocator>, no_map> compress()
			requires std::is_nothrow_move_constructible_v<T>
		{
			static constexpr bool c_return_map = std::same_as<t_returnMap, return_map>;
			std::conditional_t<c_return_map, remap_map<Allocator>, no_map> map;

			uint32_t last_index = 0;

			if (m_size)
			{
				last_index = cback().m_data - m_data;
				uint32_t elements_to_move = 0;

				if constexpr (c_return_map)
				{
					uint32_t skip_slots = 0;
					uint32_t counted_elements = 0;

					for (const uint32_t* current_index = m_skip_data; skip_slots < m_size - counted_elements;)
					{
						++current_index;
						++counted_elements;
						skip_slots += *current_index;
						current_index += *current_index;
					}

					elements_to_move = m_size - counted_elements;
				}
				else
				{
					elements_to_move = last_index - m_size;
				}

				if (elements_to_move)
				{
					if constexpr (c_return_map)
					{
						size_t pow_2_elements_to_move = std::bit_ceil(elements_to_move);

						if (static_cast<float>(elements_to_move) / static_cast<float>(pow_2_elements_to_move) > 0.6f)
						{
							pow_2_elements_to_move *= 2;
						}

						map.allocate(pow_2_elements_to_move);
					}

					for (uint32_t current_index = 1; current_index < last_index; ++current_index)
					{
						if (m_skip_data[current_index])
						{
							::new(&m_data[current_index].value) T(std::move(m_data[last_index].value));
							m_data[last_index].value.~T();

							if constexpr (c_generational)
							{
								m_data[current_index].generation = m_data[last_index].generation;
							}

							if constexpr (c_return_map)
							{
								map.insert(last_index - 1, current_index - 1);
							}

							while (m_skip_data[--last_index] && last_index > current_index) {}
						}
					}
				}

				decommit_pages_(m_size);
			}
			else
			{
				decommit_pages_(last_index);
			}

			m_high_water_mark = m_size;
			m_data[0].indices = FreeListIndices{};

			if constexpr (c_generational)
			{
				memset(m_data + m_high_water_mark + 1, 0, (m_end_data - m_data - m_high_water_mark - 1) * sizeof(Node));
			}

			memset(m_skip_data, 0, get_skip_array_bytes_());

			return map;
		}


		void shrink_to_fit() noexcept
		{
			uint32_t index = 0;

			m_data[0].indices = FreeListIndices{};

			if (m_size)
			{
				index = cback().m_data - m_data;

				decommit_pages_(index);

				if constexpr (c_generational)
				{
					memset(m_data + index + 1, 0, (m_end_data - m_data - index - 1) * sizeof(Node));
				}

				memset(m_skip_data + index + 1, 0, get_skip_array_bytes_() - (index + 1) * sizeof(uint32_t));

				uint32_t new_free_list_index = 0;
				for (const uint32_t* current_index = m_skip_data + index; current_index != m_skip_data;)
				{
					--current_index;
					current_index -= *current_index;

					const uint32_t free_list_index = current_index - m_skip_data;

					if (m_skip_data[free_list_index + 1])
					{
						m_data[free_list_index + 1].indices.next = new_free_list_index;
						m_data[new_free_list_index].indices.previous = free_list_index + 1;
						new_free_list_index = free_list_index + 1;
					}
				}

				m_data[new_free_list_index].indices.previous = 0;
				m_data[0].indices.next = new_free_list_index;

				m_high_water_mark = index;

				return;
			}

			decommit_pages_(index);

			m_high_water_mark = 0;
			m_data[0].indices = FreeListIndices{};

			memset(m_skip_data, 0, get_skip_array_bytes_());
		}

		// ITERATORS
		[[nodiscard]] iterator begin() noexcept
		{
			return iterator(m_data + m_skip_data[1] + 1, m_skip_data + m_skip_data[1] + 1);
		}


		[[nodiscard]] iterator end() noexcept
		{
			return iterator(m_data + m_high_water_mark + 1, m_skip_data + m_high_water_mark + 1);
		}


		[[nodiscard]] iterator back() noexcept
		{
			iterator to_return(m_data + m_high_water_mark + 1, m_skip_data + m_high_water_mark + 1);

			return m_size ? --to_return : to_return;
		}


		[[nodiscard]] const_iterator begin() const noexcept
		{
			return const_iterator(m_data + m_skip_data[1] + 1, m_skip_data + m_skip_data[1] + 1);
		}


		[[nodiscard]] const_iterator end() const noexcept
		{
			return const_iterator(m_data + m_high_water_mark + 1, m_skip_data + m_high_water_mark + 1);
		}


		[[nodiscard]] const_iterator back() const noexcept
		{
			const_iterator to_return(m_data + m_high_water_mark + 1, m_skip_data + m_high_water_mark + 1);

			return m_size ? --to_return : to_return;
		}


		[[nodiscard]] const_iterator cbegin() const noexcept
		{
			return begin();
		}


		[[nodiscard]] const_iterator cend() const noexcept
		{
			return end();
		}


		[[nodiscard]] const_iterator cback() const noexcept
		{
			return back();
		}


		[[nodiscard]] branched_iterator begin_branched() noexcept
		{
			return branched_iterator(m_data + m_skip_data[1] + 1, m_skip_data + m_skip_data[1] + 1);
		}


		[[nodiscard]] branched_iterator end_branched() noexcept
		{
			return branched_iterator(m_data + m_high_water_mark + 1, m_skip_data + m_high_water_mark + 1);
		}


		[[nodiscard]] branched_iterator back_branched() noexcept
		{
			branched_iterator to_return(m_data + m_high_water_mark + 1, m_skip_data + m_high_water_mark + 1);

			return m_size ? --to_return : to_return;
		}


		[[nodiscard]] const_branched_iterator begin_branched() const noexcept
		{
			return const_branched_iterator(m_data + m_skip_data[1] + 1, m_skip_data + m_skip_data[1] + 1);
		}


		[[nodiscard]] const_branched_iterator end_branched() const noexcept
		{
			return const_branched_iterator(m_data + m_high_water_mark + 1, m_skip_data + m_high_water_mark + 1);
		}


		[[nodiscard]] const_branched_iterator back_branched() const noexcept
		{
			const_branched_iterator to_return(m_data + m_high_water_mark + 1, m_skip_data + m_high_water_mark + 1);

			return m_size ? --to_return : to_return;
		}


		[[nodiscard]] const_branched_iterator cbegin_branched() const noexcept
		{
			return begin_branched();
		}


		[[nodiscard]] const_branched_iterator cend_branched() const noexcept
		{
			return end_branched();
		}


		[[nodiscard]] const_branched_iterator cback_branched() const noexcept
		{
			return back_branched();
		}

	private: // IMPLEMENTATION
		void allocate_(uint32_t p_reserve_count)
		{
			[[maybe_unused]] static const bool _ = []() noexcept -> bool
			{
				implementation::get_system_page_data();

				sm_reservedBytes = (t_VM_reserve_elements + 1) * sizeof(Node);
				sm_reservedBytes = implementation::align(sm_reservedBytes, implementation::OS_PAGE_SIZE);
				sm_skipReservedBytes = (sm_reservedBytes / sizeof(Node) + 1) * sizeof(uint32_t);
				sm_skipReservedBytes = implementation::align(sm_skipReservedBytes, implementation::OS_PAGE_SIZE);

				return false;
			}();

			const size_t elements_to_reserve = std::clamp(static_cast<size_t>(p_reserve_count), 1ULL, t_VM_reserve_elements + 1);

			size_t reserve_size = (elements_to_reserve + 1) * sizeof(Node);
			reserve_size = implementation::align(reserve_size, implementation::OS_PAGE_SIZE);
			size_t skip_reserve_size = (reserve_size / sizeof(Node) + 1) * sizeof(uint32_t);
			skip_reserve_size = implementation::align(skip_reserve_size, implementation::OS_PAGE_SIZE);

			m_page_count = static_cast<uint32_t>(reserve_size / implementation::OS_PAGE_SIZE);

			m_data = static_cast<Node*>(VirtualAlloc(NULL, sm_reservedBytes, MEM_RESERVE, PAGE_READWRITE));
			m_skip_data = static_cast<uint32_t*>(VirtualAlloc(NULL, sm_skipReservedBytes, MEM_RESERVE, PAGE_READWRITE));

			if (!m_data || !m_skip_data) [[unlikely]]
			{
				goto FAIL;
			}
			if (!VirtualAlloc(m_data, reserve_size, MEM_COMMIT, PAGE_READWRITE) ||
				!VirtualAlloc(m_skip_data, skip_reserve_size, MEM_COMMIT, PAGE_READWRITE)) [[unlikely]]
			{
				goto FAIL;
			}

			m_end_data = m_data + reserve_size / sizeof(Node);

			::new(&m_data[0].indices) FreeListIndices{};

			return;

		FAIL:
			if (m_data)
			{
				if (!VirtualFree(m_data, 0, MEM_RELEASE))
				{
					std::abort();
				}
			}
			if (m_skip_data)
			{
				if (!VirtualFree(m_skip_data, 0, MEM_RELEASE))
				{
					std::abort();
				}
			}

			throw std::bad_alloc();
		}


		template<bool t_enableLastIndex = false>
		void deallocate_(uint32_t lastIndex = 0) noexcept
		{
			if (m_data)
			{
				for (T& element : *this)
				{
					element.~T();

					if constexpr (t_enableLastIndex)
					{
						if (node_of_(&element) - m_data >= lastIndex)
						{
							break;
						}
					}
				}

				if (!VirtualFree(m_data, 0, MEM_RELEASE) ||
					!VirtualFree(m_skip_data, 0, MEM_RELEASE)) [[unlikely]]
				{
					std::abort();
				}
			}
		}


		void grow_(uint32_t p_new_page_count)
		{
			const uint32_t max_page_count = static_cast<uint32_t>(sm_reservedBytes / implementation::OS_PAGE_SIZE);

			m_page_count = std::min(p_new_page_count, max_page_count);
			m_end_data = m_data + m_page_count * implementation::OS_PAGE_SIZE / sizeof(Node);

			if (!VirtualAlloc(m_data, (m_page_count * implementation::OS_PAGE_SIZE), MEM_COMMIT, PAGE_READWRITE) ||
				!VirtualAlloc(m_skip_data, get_skip_array_bytes_(), MEM_COMMIT, PAGE_READWRITE)) [[unlikely]]
			{
				throw std::bad_alloc();
			}
		}


		void copy_stable_vector_(const stable_vector& other)
		{
			m_size = other.m_size;
			m_high_water_mark = other.m_high_water_mark;

			allocate_(m_high_water_mark);

			memcpy(m_skip_data, other.m_skip_data, (m_high_water_mark + 1) * sizeof(uint32_t));

			if constexpr (std::is_trivially_copyable_v<T>)
			{
				memcpy(m_data, other.m_data, (m_high_water_mark + 1) * sizeof(Node));
			}
			else
			{
				size_t current_index = 1;

				::new(&m_data[0].indices) FreeListIndices(other.m_data[0].indices);

				try
				{
					for (; current_index != m_high_water_mark + 1; ++current_index)
					{
						if (m_skip_data[current_index])
						{
							::new(&m_data[current_index].indices) FreeListIndices(other.m_data[current_index].indices);
						}
						else
						{
							::new(&m_data[current_index].value) T(other.m_data[current_index].value);
						}

						if constexpr (c_generational)
						{
							m_data[current_index].generation = other.m_data[current_index].generation;
						}
					}
				}
				catch (...)
				{
					deallocate_<true>(current_index - 1);

					throw;
				}
			}
		}


		void decommit_pages_(uint32_t p_index) noexcept
		{
			size_t bytes_used = (p_index + 1) * sizeof(Node);
			bytes_used = implementation::align(bytes_used, implementation::OS_PAGE_SIZE);
			const uint32_t pages_used = bytes_used / implementation::OS_PAGE_SIZE;
			const size_t bytes_to_decommit = (m_page_count - pages_used) * implementation::OS_PAGE_SIZE;

			const size_t previous_skip_array_bytes = get_skip_array_bytes_();

			m_page_count = pages_used;

			const size_t skip_array_bytes = get_skip_array_bytes_();
			const size_t skip_array_bytes_to_decommit = (previous_skip_array_bytes - skip_array_bytes);

			if (bytes_to_decommit)
			{
				if (!VirtualFree(reinterpret_cast<char*>(m_data) + bytes_used, bytes_to_decommit, MEM_DECOMMIT)) [[unlikely]]
				{
					std::abort();
				}
			}
			if (skip_array_bytes_to_decommit)
			{
				if (!VirtualFree(reinterpret_cast<char*>(m_skip_data) + skip_array_bytes, skip_array_bytes_to_decommit, MEM_DECOMMIT)) [[unlikely]]
				{
					std::abort();
				}
			}

			m_end_data = m_data + bytes_used / sizeof(Node);
		}


		[[nodiscard]] size_t get_skip_array_bytes_() const noexcept
		{
			const size_t skip_array_bytes = (m_page_count * implementation::OS_PAGE_SIZE / sizeof(Node) + 1) * sizeof(uint32_t);

			return implementation::align(skip_array_bytes, implementation::OS_PAGE_SIZE);
		}


		[[nodiscard]] Node* get_allocation_slot_()
		{
			FreeListIndices& freeList = m_data[0].indices;

			if (freeList.next) [[likely]] // Seems to prioritize codegen for heavier path
			{
				const uint32_t current_index = freeList.next;
				const uint32_t jump_size = m_skip_data[current_index];

				if (jump_size != 1) [[likely]]
				{
					m_skip_data[current_index] = jump_size - 1;
					m_skip_data[current_index + jump_size - 2] = jump_size - 1;
					m_skip_data[current_index + jump_size - 1] = 0;

					return m_data + current_index + jump_size - 1;
				}
				else [[unlikely]]
				{
					m_skip_data[current_index] = 0;

					const uint32_t next_free_list_index = m_data[current_index].indices.next;

					freeList.next = next_free_list_index;
					m_data[next_free_list_index].indices.previous = 0;

					return m_data + current_index;
				}
			}
			else if (m_data + m_high_water_mark + 1 == m_end_data) [[unlikely]]
			{
				grow_(m_page_count * 2);
			}

			++m_high_water_mark;

			return m_data + m_high_water_mark;
		}


		[[nodiscard]] Node* get_end_allocation_slot_()
		{
			if (m_data + m_high_water_mark + 1 == m_end_data) [[unlikely]]
			{
				grow_(m_page_count * 2);
			}

			++m_high_water_mark;

			return m_data + m_high_water_mark;
		}


		[[nodiscard]] Node* get_unchecked_allocation_slot_() noexcept
		{
			++m_high_water_mark;

			return m_data + m_high_water_mark;
		}


		void update_skip_array_(uint32_t p_current_skip_node_index) noexcept
		{
			const uint32_t left_skip_size = *(m_skip_data + p_current_skip_node_index - 1);
			uint32_t* const current_skip_node = m_skip_data + p_current_skip_node_index;
			const uint32_t right_skip_size = m_skip_data[p_current_skip_node_index + 1];

			if (left_skip_size) [[unlikely]]
			{
				if (right_skip_size) [[likely]] // Seems to priotitize codegen for heavier paths, does not reflect actual branch chanes
				{
					const uint32_t total_skip_size = left_skip_size + right_skip_size + 1;

					*(current_skip_node - left_skip_size) = total_skip_size;
					*current_skip_node = total_skip_size;
					current_skip_node[right_skip_size] = total_skip_size;

					const FreeListIndices indices = m_data[p_current_skip_node_index + 1].indices;

					m_data[indices.previous].indices.next = indices.next;
					m_data[indices.next].indices.previous = indices.previous;

					return;
				}

				*(current_skip_node - left_skip_size) = left_skip_size + 1;
				*current_skip_node = left_skip_size + 1;
			}
			else if (right_skip_size) [[likely]]
			{
				*current_skip_node = right_skip_size + 1;
				current_skip_node[right_skip_size] = right_skip_size + 1;

				const FreeListIndices indices = m_data[p_current_skip_node_index + 1].indices;

				::new(&m_data[p_current_skip_node_index].indices) FreeListIndices(indices);

				m_data[indices.previous].indices.next = p_current_skip_node_index;
				m_data[indices.next].indices.previous = p_current_skip_node_index;
			}
			else [[unlikely]]
			{
				*current_skip_node = 1;

				FreeListIndices& free_list = m_data[0].indices;

				::new(&m_data[p_current_skip_node_index].indices) FreeListIndices{ free_list.next, 0 };

				m_data[free_list.next].indices.previous = p_current_skip_node_index;

				free_list.next = p_current_skip_node_index;
			}
		}


		[[nodiscard]] constexpr static Node* node_of_(T* element) noexcept
		{
			return reinterpret_cast<Node*>(reinterpret_cast<char*>(element) - offsetof(Node, value));
		}


		[[nodiscard]] constexpr static const Node* node_of_(const T* element) noexcept
		{
			return reinterpret_cast<const Node*>(reinterpret_cast<char*>(element) - offsetof(Node, value));
		}

	private: // MEMBERS
		inline static size_t sm_reservedBytes;
		inline static size_t sm_skipReservedBytes;

		Node* m_data = nullptr;
		Node* m_end_data = nullptr;
		uint32_t* m_skip_data = nullptr;
		uint32_t m_page_count;
		uint32_t m_high_water_mark = 0;
		uint32_t m_size = 0;
	};



	// ITERATOR
	namespace implementation
	{
		template<class T, size_t t_elements, use_generations_concept t_use_generations, const_iterator_concept t_is_const, iterator_branch_concept>
		class stable_vector_iterator_base
		{
		protected:
			template<class, size_t t_VM_reserve_elements, use_generations_concept>
				requires (t_VM_reserve_elements <= UINT32_MAX)
			friend class stable_vector;

		public:
			using value_type = T;
			using difference_type = std::ptrdiff_t;
			using iterator_category = std::bidirectional_iterator_tag;

		protected:
			constexpr static bool c_constant = std::same_as<t_is_const, is_const>;

			using ValueType = std::conditional_t<c_constant, const T, T>;
			using DataValueType = std::conditional_t<c_constant, typename const stable_vector<T, t_elements, t_use_generations>::Node*, typename stable_vector<T, t_elements, t_use_generations>::Node*>;
			using SkipValueType = std::conditional_t<c_constant, const uint32_t*, uint32_t*>;

		public:
			stable_vector_iterator_base() noexcept = default;


			stable_vector_iterator_base(DataValueType p_data, SkipValueType p_skip_ptr) noexcept : m_data(p_data), m_skip_ptr(p_skip_ptr) {}

		public:
			[[nodiscard]] ValueType& operator*() const noexcept
			{
				return m_data->value;
			}


			[[nodiscard]] ValueType* operator->() const noexcept
			{
				return reinterpret_cast<ValueType*>(&m_data->value);
			}


			bool operator==(const stable_vector_iterator_base& other) const noexcept { return m_data == other.m_data; }
			bool operator!=(const stable_vector_iterator_base& other) const noexcept { return m_data != other.m_data; }
			bool operator>(const stable_vector_iterator_base& other) const noexcept { return m_data > other.m_data; }
			bool operator<(const stable_vector_iterator_base& other) const noexcept { return m_data < other.m_data; }
			bool operator>=(const stable_vector_iterator_base& other) const noexcept { return m_data >= other.m_data; }
			bool operator<=(const stable_vector_iterator_base& other) const noexcept { return m_data <= other.m_data; }

		protected:
			DataValueType m_data;
			SkipValueType m_skip_ptr;
		};
	}


	template<class T, size_t t_elements, use_generations_concept t_use_generations, const_iterator_concept t_is_const>
	class stable_vector_iterator<T, t_elements, t_use_generations, t_is_const, branchless> : public implementation::stable_vector_iterator_base<T, t_elements, t_use_generations, t_is_const, branchless>
	{
	private:
		using Base = implementation::stable_vector_iterator_base<T, t_elements, t_use_generations, t_is_const, branchless>;

	public:
		using Base::Base;

	public:
		stable_vector_iterator& operator++() noexcept
		{
			++this->m_skip_ptr;
			++this->m_data;
			this->m_data += *this->m_skip_ptr;
			this->m_skip_ptr += *this->m_skip_ptr;

			return *this;
		}


		stable_vector_iterator operator++(int) noexcept
		{
			const stable_vector_iterator other{ *this };
			++*this;

			return other;
		}


		stable_vector_iterator& operator--() noexcept
		{
			--this->m_skip_ptr;
			--this->m_data;
			this->m_data -= *this->m_skip_ptr;
			this->m_skip_ptr -= *this->m_skip_ptr;

			return *this;
		}


		stable_vector_iterator operator--(int) noexcept
		{
			const stable_vector_iterator other{ *this };
			--*this;

			return other;
		}
	};



	template<class T, size_t t_elements, use_generations_concept t_use_generations, const_iterator_concept t_is_const>
	class stable_vector_iterator<T, t_elements, t_use_generations, t_is_const, branched> : public implementation::stable_vector_iterator_base<T, t_elements, t_use_generations, t_is_const, branched>
	{
	private:
		using Base = implementation::stable_vector_iterator_base<T, t_elements, t_use_generations, t_is_const, branched>;

	public:
		using Base::Base;

	public:
		stable_vector_iterator& operator++() noexcept
		{
			do
			{
				++this->m_skip_ptr;
				++this->m_data;
			} while (*this->m_skip_ptr);

			return *this;
		}


		stable_vector_iterator operator++(int) noexcept
		{
			const stable_vector_iterator other{ *this };
			++*this;

			return other;
		}


		stable_vector_iterator& operator--() noexcept
		{
			do
			{
				--this->m_skip_ptr;
				--this->m_data;
			} while (*this->m_skip_ptr);

			return *this;
		}


		stable_vector_iterator operator--(int) noexcept
		{
			const stable_vector_iterator other{ *this };
			--*this;

			return other;
		}
	};



	// REMAP MAP, simplified, never needs to grow or make checks
	template<class Allocator>
	class remap_map
	{
	private:
		struct RemapNode
		{
			int32_t psl = -1;
			uint32_t key = 0;
			uint32_t value = 0;

			[[nodiscard]] bool is_empty() const { return psl == -1; };
		};

	private:
		using NodeAllocator = typename std::allocator_traits<Allocator>::template rebind_alloc<RemapNode>;

	private:
		remap_map() noexcept = default;

		remap_map(const Allocator& p_alloc) noexcept : m_state(p_alloc) {}

	public:
		remap_map(const remap_map& p_other) : m_state(p_other.m_state)
		{
			m_state.data = m_state.allocate(m_state.size);
			memcpy(m_state.data, p_other.m_state.data, m_state.size * sizeof(RemapNode));
		}


		remap_map& operator=(const remap_map& p_other)
		{
			if (this != &p_other)
			{
				CompressedState new_state = p_other.m_state;
				new_state.data = new_state.allocate(new_state.size);

				m_state.deallocate(m_state.data, m_state.size);
				memcpy(new_state.data, p_other.m_state.data, new_state.size * sizeof(RemapNode));

				m_state = std::move(new_state);
			}

			return *this;
		}


		remap_map(remap_map&& p_other) noexcept : m_state(std::move(p_other.m_state))
		{
			p_other.m_state.data = nullptr;
			p_other.m_state.size = 0;
		}


		remap_map& operator=(remap_map&& p_other) noexcept
		{
			if (this != &p_other)
			{
				m_state.deallocate(m_state.data, m_state.size);

				m_state = std::move(p_other.m_state);

				p_other.m_state.data = nullptr;
				p_other.m_state.size = 0;
			}

			return *this;
		}


		~remap_map() noexcept
		{
			m_state.deallocate(m_state.data, m_state.size);
		}

	private:
		void allocate(size_t p_element_count)
		{
			m_state.size = p_element_count;

			m_state.data = m_state.allocate(m_state.size);

			for (size_t index = 0; index != m_state.size; ++index)
			{
				m_state.data[index] = RemapNode();
			}
		}


		void insert(uint32_t p_key, uint32_t p_value) noexcept
		{
			size_t index = std::hash<uint32_t>()(p_key) & (m_state.size - 1);
			int32_t psl = 0;

			while (true)
			{
				if (m_state.data[index].is_empty())
				{
					m_state.data[index].psl = psl;
					m_state.data[index].key = p_key;
					m_state.data[index].value = p_value;

					return;
				}
				else if (psl > m_state.data[index].psl)
				{
					std::swap(psl, m_state.data[index].psl);
					std::swap(p_key, m_state.data[index].key);
					std::swap(p_value, m_state.data[index].value);
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
		[[nodiscard]] uint32_t find(uint32_t p_key) const noexcept
		{
			const size_t start = std::hash<uint32_t>()(p_key) & (m_state.size - 1);
			size_t index = start;
			int32_t psl = 0;

			while (p_key != m_state.data[index].key)
			{
				if (psl > m_state.data[index].psl)
				{
					return p_key;
				}

				++psl;
				++index;

				if (index == m_state.size)
				{
					index = 0;
				}
				if (index == start)
				{
					return p_key;
				}
			}

			return m_state.data[index].value;
		}


		[[nodiscard]] bool is_empty() const noexcept
		{
			return !m_state.size;
		}

	private:
		template<class, size_t t_VM_reserve_elements, use_generations_concept>
			requires (t_VM_reserve_elements <= UINT32_MAX)
		friend class stable_vector;


		struct CompressedState : public NodeAllocator
		{
			RemapNode* data = nullptr;
			size_t size = 0;
		};

		CompressedState m_state;
	};
}
