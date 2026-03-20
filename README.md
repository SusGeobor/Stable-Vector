Basically, plf::colony but allocated using VirtualAlloc

https://www.youtube.com/watch?v=ukZrJPGZqRs

Freelist was FILO stack, but I changed it to blocks and boundary tagging for random case performance. 

Handles were made from T* into uint32_t so handle + generation can be copied in 1 register.

Q: Why VitualAlloc?

A: avoid worst case insert reallocation, iterators and pointers remain valid on erase, iterators are invalidated by erase though

Q: Why uint32_t?

A: locks the container to 4b max elements, next lowest size would be 65k

Q: why parallel skip array?

A: singular performance test showed it slightly faster, but I might weave it with data* in the future

Q: algorithmic complexity?

A:
O(N) iteration
O(1) insert
O(1) erase
O(1) validity checks

Q: vs plf::colony?

A:Very rough tests showed:
iteration 2% - 30% faster
insert 30% faster - 20% slower (will try to improve)
erase 100% - 150% faster
allocation 15% - 30% faster (may improve)
validating handles; colony cannot validate handles in O(1) time. this container tends to be an order of magnitude or a few orders of magnitude faster here. This was a deliberate choice, by making it contiguous and reuse slots rather than shrink, validity always requires 1 O(1) check.
Lookup performance, both 1 indirection, direct pointer lookups
handle size, 4 bytes without generations, 8 bytes with generations. Colony handles are 24 bytes.
