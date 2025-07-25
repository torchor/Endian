//
//  main.cpp
//  C++Timer
//
//  Created by Matthew on 2024/10/19.
//
#ifndef __CONDINTION_TIMER__
#define __CONDINTION_TIMER__

#include <iostream>
#include <map>
#include <functional>
#include <thread>
#include <condition_variable>
#include <chrono>
#include <iomanip>
#include <ctime>
#include <unordered_map>

namespace timer{
	using milliseconds_t = std::chrono::milliseconds;
	using timer_id =  uint64_t;///高48位表示距离base_time时间戳，低16位区分同一个时间，的不同Timer
	using callback = std::function<milliseconds_t(void)>;///返回>0，定时器继续；否则 取消定时器

	using callback_until_timeup = std::function<void(uint32_t index,bool isLastTime)>;///返回>0，定时器继续；否则 取消定时器

class Timer {
public:
	Timer() :base_time(std::chrono::steady_clock::now()), keySufix(0) {
		auto thread = std::thread([this]() {
			std::unique_lock<std::mutex> lock(m_mutex);
			do {
				m_condition.wait(lock, [this]() {
					return !m_map.empty();
				});

				auto first = m_map.begin();
				auto timerId = first->first;
				auto nd = first->second;

				auto expire_time = base_time + milliseconds_t(timerId >> (sizeof(keySufix) * 8));

				m_condition.wait_until(lock, expire_time);

				if (m_map.empty()) {
					continue;
				}

				if (timerId != m_map.begin()->first) {///醒来的时候，之前的定时器已经被删除了，开始处理下一个
					continue;
				}

				if (std::chrono::steady_clock::now() < expire_time) {///not expired yet, ensure it's timeout not a spurious wakeup
					continue;
				}

				auto next_duration = nd.call();
				m_map.erase(timerId);
				if (next_duration.count() > 0) {///setup next fire time
					nd.timeout = next_duration;
					create_timer(nd);
				}
				else {///clear timer
					index_map.erase(nd.orignal_id);
				}

			} while (true);
		});
		thread_id = thread.get_id();
		thread.detach();
	}

	timer_id scheduledTimer(const milliseconds_t timeout,const callback& call) {
		node nd;
		nd.call = call;
		nd.timeout = timeout;
		nd.orignal_id = 0;

		create_timer(nd);
		m_condition.notify_one();
		return nd.orignal_id;
	}

	timer_id scheduledTimer(const milliseconds_t interval, uint32_t repeatCount, const callback_until_timeup& call) {
		if (repeatCount <=0 || !call || interval.count() <= 0)
		{
			return 0;
		}
		auto count = 0;
		return scheduledTimer(interval, [=]() mutable{
			repeatCount--;

			auto next_timer = repeatCount <= 0 ? milliseconds_t{ 0 } : interval;
			call(++count, next_timer.count() <= 0);
			return next_timer;
		});
	}

	///每隔interval秒 执行一次，总时间total
	timer_id scheduledTimer(const milliseconds_t interval,  milliseconds_t total, const callback_until_timeup& call) {
		if (interval > total || total.count() <= 0 || interval.count() <=0 || !call)
		{
			return 0;
		}
		auto count = 0;
		return scheduledTimer(interval, [=]() mutable{
			auto next_timer =  (total >= interval) ? interval : total;
			total -= interval;
			if (total.count() <= 0)
			{
				next_timer = milliseconds_t{0};
			}
			call(++count, next_timer.count() <= 0);
			return  next_timer;
		});
	}

	void  removeTimer(timer_id id) {
		auto isOnThread = std::this_thread::get_id() == thread_id;

		auto fn = [&]() {
			auto key = index_map.find(id);
			if (key != index_map.end()) {
				m_map.erase(key->second);
				index_map.erase(id);
			}
		};

		if (!isOnThread) {
			std::lock_guard<std::mutex> guard(m_mutex);
			fn();
		}
		else {
			fn();
		}
	}

private:
	struct node {
		timer_id orignal_id;
		callback call;
		milliseconds_t timeout;///每隔多久触发一次
	};

	void create_timer(node& parameter) {
		auto isOnThread = std::this_thread::get_id() == thread_id;
		bool retry = true;
		timer_id newId = 0;
		while (retry) {
			retry = false;

			auto expired = std::chrono::steady_clock::now() - base_time + parameter.timeout;
			int64_t milliseconds = std::chrono::duration_cast<milliseconds_t>(expired).count();
			if (!isOnThread) {
				std::lock_guard<std::mutex> guard(m_mutex);
				keySufix++;
			}
			else {
				keySufix++;
			}
			newId = (milliseconds << (sizeof(keySufix) * 8)) | keySufix;
			if (0 == parameter.orignal_id) {///first time save as it's orginal id
				parameter.orignal_id = newId;
			}

			if (!isOnThread) {
				std::lock_guard<std::mutex> guard(m_mutex);
				if (m_map.find(newId) != m_map.end()) {///已经存在这个key, 需要重写生成一个唯一key
					retry = true;
					continue;
				}
				index_map[parameter.orignal_id] = newId;
				m_map[newId] = parameter;
			}
			else {
				if (m_map.find(newId) != m_map.end()) {///已经存在这个key, 需要重写生成一个唯一key
					retry = true;
					continue;
				}
				index_map[parameter.orignal_id] = newId;
				m_map[newId] = parameter;
			}

		}
	}


	std::mutex m_mutex;
	std::condition_variable m_condition;

	std::thread::id thread_id;
	std::map<timer_id, node> m_map;///自动按时间排序

	std::unordered_map<timer_id, timer_id> index_map;///orinalId ---> timerId

#if defined(_MSC_VER) && _MSC_VER <= 1900
	const std::chrono::time_point<std::chrono::high_resolution_clock> base_time;///for VS2013 Only,because it's too old
#else
	const std::chrono::time_point<std::chrono::steady_clock> base_time;
#endif
	
	uint16_t keySufix;///一直递增，避免同一时间生成的key 一样
};

}

#endif