// An allocator for game code (ext/*/game) whose containers outlive a stage. Game code's `new` takes
// from the game's current JKR heap, which a stage change empties, so a std::string or std::vector
// kept from one stage to the next (or made at boot) ends up pointing into memory used again. These
// take malloc's memory instead. C++03, as game code is.
//
//   typedef std::basic_string<char, std::char_traits<char>, PortHostAlloc<char> > HostString;
//   std::vector<HostString, PortHostAlloc<HostString> > names;
#ifndef PORT_HOST_ALLOC_H
#define PORT_HOST_ALLOC_H

#include <stddef.h>
#include <stdlib.h>
#include <new>
#include <string>

template <class T> class PortHostAlloc {
public:
	typedef T value_type;
	typedef T* pointer;
	typedef const T* const_pointer;
	typedef T& reference;
	typedef const T& const_reference;
	typedef size_t size_type;
	typedef ptrdiff_t difference_type;
	template <class U> struct rebind {
		typedef PortHostAlloc<U> other;
	};

	PortHostAlloc() {}
	PortHostAlloc(const PortHostAlloc&) {}
	template <class U> PortHostAlloc(const PortHostAlloc<U>&) {}

	pointer address(reference x) const { return &x; }
	const_pointer address(const_reference x) const { return &x; }
	pointer allocate(size_type n, const void* = 0)
	{
		void* p = malloc(n ? n * sizeof(T) : 1);
		if (!p)
			abort();
		return static_cast<pointer>(p);
	}
	void deallocate(pointer p, size_type) { free(p); }
	size_type max_size() const { return (size_type)-1 / sizeof(T); }
	void construct(pointer p, const T& v) { new ((void*)p) T(v); }
	void destroy(pointer p) { p->~T(); }
};

template <class T, class U> bool operator==(const PortHostAlloc<T>&, const PortHostAlloc<U>&) { return true; }
template <class T, class U> bool operator!=(const PortHostAlloc<T>&, const PortHostAlloc<U>&) { return false; }

typedef std::basic_string<char, std::char_traits<char>, PortHostAlloc<char> > PortHostString;

#endif
