//
//  safe_containers.h
//  ConcurrentContainer
//
//  Created by Matthew on 2025/03/02.
//

#ifndef __SAFE_CONTAINERS_H__
#define __SAFE_CONTAINERS_H__

#include <iostream>
#include <map>
#include <functional>
#include <mutex>
#include <array>
#include <list>
#include <set>
#include <unordered_set>
#include <unordered_map>


namespace safe {
	using  DefaultMutexType = std::mutex;

	template<typename MutexType>
	using  ReadLock = std::unique_lock<MutexType>;

	template<typename MutexType>
	using  WriteLock = std::unique_lock<MutexType>;

	template<typename T, int N>
	struct _TAG_ {
		using Type = T;
	};
	template<typename MutexType>
	using _ReadLock_ = _TAG_<ReadLock<MutexType>, 0>;

	template<typename MutexType>
	using _WriteLock_ = _TAG_<WriteLock<MutexType>, 1>;

	template<typename Type,typename TAG>
	struct locked_ref;

	template<typename Type, typename MutexType>
	struct locked_ref<Type,_ReadLock_<MutexType>>
	{
		using LockType = typename  _ReadLock_<MutexType>::Type;
		const Type * operator->() {///Read Only
			return &ref;
		};
		inline const Type & get() {///Read Only
			return ref;
		};
		locked_ref& operator=(const locked_ref&) = delete;
		locked_ref(const locked_ref&other) = delete;
		locked_ref(locked_ref &&other) :ref(other.ref), lock(std::move(other.lock)) {}
		locked_ref(MutexType&_mutex, Type &_container) :lock(_mutex), ref(_container) {}
	private:
		LockType lock;
		const Type & ref;///Read Only
	};

	template<typename Type, typename MutexType>
	struct locked_ref<Type,_WriteLock_<MutexType>>
	{
		using LockType = typename  _WriteLock_<MutexType>::Type;
		Type * operator->() {///Write
			return &ref;
		};
		inline Type & get() {///Write
			return ref;
		};
		locked_ref& operator=(const locked_ref&) = delete;
		locked_ref(const locked_ref&other) = delete;
		locked_ref(locked_ref &&other) :ref(other.ref), lock(std::move(other.lock)) {}
		locked_ref(MutexType&_mutex, Type &_container) :lock(_mutex), ref(_container) {}
	private:
		LockType lock;
		Type & ref;///Write
	};

	template<typename M,typename MutexType = DefaultMutexType>
	struct obj {
		using Type = M;

		template<typename ...T>
		obj(T&& ...para):_container(std::forward<T>(para)...) {}
		///安全的对map进行任何修改
		inline void write(const std::function<void(Type &)> &fn) {
			WriteLock<MutexType> write_lock(_mutex);
			fn(_container);
		}

		inline void read(const std::function<void(const Type &)> &fn) {
			ReadLock<MutexType> read_lock(_mutex);
			fn(_container);
		}

		inline locked_ref<Type, _ReadLock_<MutexType>> safe_read_obj() {
			return locked_ref<Type, _ReadLock_<MutexType>>(_mutex,_container);
		}

		inline locked_ref<Type, _WriteLock_<MutexType>> safe_write_obj() {
			return locked_ref<Type, _WriteLock_<MutexType>>(_mutex, _container);
		}

		inline obj& operator=(const Type& other) {
			WriteLock<MutexType> write_lock(_mutex);
			if (&_container != &other)
			{
				_container = other;
			}
			return *this;
		}

		inline obj& operator=(Type&& other) {
			WriteLock<MutexType> write_lock(_mutex);
			if (&_container != &other)
			{
				_container = std::move(other);
			}
			return *this;
		}

		inline  operator Type() {
			ReadLock<MutexType> read_lock(_mutex);
			return _container;
		}

		MutexType _mutex;
		Type _container;
	};

	template<typename M,typename E, typename MutexType>
	struct _base_container_:public obj<M, MutexType> {

		using SUPER = obj<M, MutexType>;

		using ContainerType = M;
		using Element = E;

		template<typename T>
		using _SizeType_ = decltype( std::declval<T>().size() );

		template<typename T>
		using Iterable = decltype(_SizeType_<T>{}, std::declval<T>().begin(), std::declval<T>().end());

		using SizeType = _SizeType_<ContainerType>;
		using IterType = Iterable<ContainerType>;

		
		
		///安全遍历，返回true则停止，否则遍历所有元素
		template<typename LockType = ReadLock<MutexType>>
		void visit(const std::function<bool(const Element&)> &fn) {
			LockType lock(SUPER::_mutex);
			for (auto &it : SUPER::_container)
			{
				auto stop = fn(it);
				if (stop)
				{
					return;
				}
			}
		}
		
		using SUPER::operator=;

		///删除满足条件的元素
		void filterItemsWhen(const std::function<bool(IterType)> &match) {
			WriteLock<MutexType> write_lock(SUPER::_mutex);
			for (auto it = SUPER::_container.begin(); it != SUPER::_container.end();)
			{
				auto shouldRemove = match(it);
				if (shouldRemove)
				{
					it = SUPER::_container.erase(it);
				}
				else {
					it++;
				}
			}
		}

		inline void swap(ContainerType& to) {
			WriteLock<MutexType> write_lock(SUPER::_mutex);
			SUPER::_container.swap(to);
		}

		inline SizeType size() {
			ReadLock<MutexType> read_lock(SUPER::_mutex);
			return SUPER::_container.size();
		}

		inline bool empty() {
			ReadLock<MutexType> read_lock(SUPER::_mutex);
			return SUPER::_container.empty();
		}

		inline void clear() {
			WriteLock<MutexType> write_lock(SUPER::_mutex);
			SUPER::_container.clear();
		}
		
	};

	template<typename M, typename K, typename V, typename E, typename MutexType>
	class _base_map_ :public _base_container_<M,E,MutexType> {
	public:
		using BASE = obj<M, MutexType>;
		using SUPER = _base_container_<M, E, MutexType>;

		template<typename LockType = ReadLock<MutexType>>
		void find(const K &key, const std::function<void(typename SUPER::IterType, bool find)> &match) {
			LockType lock(BASE::_mutex);
			auto it = BASE::_container.find(key);
			match(it, it != BASE::_container.end());
		}

		inline bool insert_if_not_exist(const K &key, const V&value) {
			WriteLock<MutexType> write_lock(BASE::_mutex);
			if (BASE::_container.find(key) == BASE::_container.end())
			{
				BASE::_container.emplace(key, value);
				return true;
			}
			return false;
		}

		using BASE::operator=;

		inline void insert(const K &key, const V&value) {
			WriteLock<MutexType> write_lock(BASE::_mutex);
			BASE::_container.emplace(key, value);
		}

		template<typename T,typename = typename SUPER:: template  Iterable<T>>
		inline void erase(const T &elements) {
			WriteLock<MutexType> write_lock(BASE::_mutex);
			for (auto&&key: elements)
			{
				BASE::_container.erase(key);
			}
		}

		inline void erase(const K &key) {
			WriteLock<MutexType> write_lock(BASE::_mutex);
			BASE::_container.erase(key);
		}

		inline bool find(const K &key) {
			ReadLock<MutexType> read_lock(BASE::_mutex);
			return BASE::_container.find(key) != BASE::_container.end();
		}
	};


	template<typename K, typename V, typename MutexType = DefaultMutexType, typename _Pr = std::less<K>, typename _Alloc = std::allocator<std::pair<const K, V> > >
	using map = _base_map_<std::map<K, V, _Pr, _Alloc>, K, V, std::pair<const K, V>,MutexType>;

	template<class _Kty,
		class _Ty,
		class _Pr = std::less<_Kty>,
		class _Alloc = std::allocator<std::pair<const _Kty, _Ty> > , typename MutexType=DefaultMutexType>
	using multimap = _base_map_<std::multimap<_Kty, _Ty, _Pr, _Alloc>, _Kty, _Ty, std::pair<const _Kty, _Ty>,MutexType>;

	template<class _Kty,
		class _Ty,  typename MutexType = DefaultMutexType,
		class _Hasher = std::hash<_Kty>,
		class _Keyeq = std::equal_to<_Kty>,
		class _Alloc = std::allocator<std::pair<const _Kty, _Ty> >>
	using unordered_map = _base_map_<std::unordered_map<_Kty, _Ty, _Hasher, _Keyeq, _Alloc>, _Kty, _Ty, std::pair<const _Kty, _Ty>,MutexType>;

	template<class _Kty,
		class _Ty, typename MutexType = DefaultMutexType,
		class _Hasher = std::hash<_Kty>,
		class _Keyeq = std::equal_to<_Kty>,
		class _Alloc = std::allocator<std::pair<const _Kty, _Ty> > >
	using unordered_multimap = _base_map_<std::unordered_multimap<_Kty, _Ty, _Hasher, _Keyeq, _Alloc>, _Kty, _Ty, std::pair<const _Kty, _Ty>,MutexType>;

	template<typename M, typename E, typename MutexType>
	class _base_set_ :public _base_container_<M,E,MutexType>
	{
	public:
		using BASE = obj<M, MutexType>;
		using SUPER = _base_container_<M, E, MutexType>;
		

		template<typename LockType = ReadLock<MutexType>>
		void find(const E &element, const std::function<void(typename SUPER::IterType, bool find)> &match) {
			LockType lock(BASE::_mutex);
			auto it = BASE::_container.find(element);
			match(it, it != BASE::_container.end());
		}

		inline bool insert_if_not_exist(const E &element) {
			WriteLock<MutexType> write_lock(BASE::_mutex);
			if (BASE::_container.find(element) == BASE::_container.end())
			{
				BASE::_container.insert(element);
				return true;
			}
			return false;
		}

		template<typename T>
		inline void insert(const T &elements) {
			WriteLock<MutexType> write_lock(BASE::_mutex);
			BASE::_container.insert(elements.begin(), elements.end());
		}

		inline void insert(const E &element) {
			WriteLock<MutexType> write_lock(BASE::_mutex);
			BASE::_container.insert(element);
		}

		template<typename T, typename = typename SUPER:: template  Iterable<T>>
		inline void erase(const T &elements) {
			WriteLock<MutexType> write_lock(BASE::_mutex);
			for (auto&& key:elements)
			{
				BASE::_container.erase(key);
			}
		}

		inline void erase(const E &element) {
			WriteLock<MutexType> write_lock(BASE::_mutex);
			BASE::_container.erase(element);
		}
		
		inline bool find(const E &element) {
			ReadLock<MutexType> read_lock(BASE::_mutex);
			return BASE::_container.find(element) != BASE::_container.end();
		}
	};


	template<class _Kty, typename MutexType = DefaultMutexType,
		class _Pr = std::less<_Kty>,
		class _Alloc = std::allocator<_Kty> >
	using set = _base_set_<std::set<_Kty,_Pr,_Alloc>, _Kty,MutexType>;

	template<class _Kty, typename MutexType = DefaultMutexType,
		class _Pr = std::less<_Kty>,
		class _Alloc = std::allocator<_Kty>  >
	using multiset = _base_set_<std::multiset<_Kty, _Pr, _Alloc>, _Kty,MutexType>;

	template<class _Kty,  typename MutexType = DefaultMutexType,
		class _Hasher = std::hash<_Kty>,
		class _Keyeq = std::equal_to<_Kty>,
		class _Alloc = std::allocator<_Kty>>
	using unordered_set = _base_set_<std::unordered_set<_Kty, _Hasher, _Keyeq, _Alloc>, _Kty,MutexType>;


	template<class _Kty,  typename MutexType = DefaultMutexType,
		class _Hasher = std::hash<_Kty>,
		class _Keyeq = std::equal_to<_Kty>,
		class _Alloc = std::allocator<_Kty> >
	using unordered_multiset = _base_set_<std::unordered_multiset<_Kty, _Hasher, _Keyeq, _Alloc>, _Kty,MutexType>;

	template<class _Ty, typename MutexType = DefaultMutexType,class _Alloc = std::allocator<_Ty> >
	using vector = _base_container_<std::vector<_Ty,_Alloc>, _Ty,MutexType>;

	template<class _Ty, typename MutexType = DefaultMutexType,	class _Alloc = std::allocator<_Ty> >
	using list = _base_container_<std::list<_Ty,_Alloc>,_Ty,MutexType>;

	template<class _Ty,size_t _Size, typename MutexType = DefaultMutexType>
	using  array = _base_container_<std::array<_Ty,_Size>,_Ty,MutexType>;
}



#endif

