#include "../src/feats/firstseen.hpp"

#include <cstdio>

namespace
{
int failures = 0;

void check(bool condition, const char* message)
{
	if (condition)
		std::printf("ok   %s\n", message);
	else
	{
		std::printf("FAIL %s\n", message);
		++failures;
	}
}
}

int main()
{
	FirstSeen::TimestampMap remembered{{10, 100}};
	std::unordered_set<AppId_t> active{10, 20};

	check(FirstSeen::reconcile(remembered, active, 200),
	      "newly discovered app changes the cache");
	check(remembered.at(10) == 100,
	      "existing first-seen timestamp is stable");
	check(remembered.at(20) == 200,
	      "new app receives current timestamp");
	check(!FirstSeen::reconcile(remembered, active, 300),
	      "unchanged active set does not rewrite the cache");

	active.erase(10);
	check(!FirstSeen::reconcile(remembered, active, 400),
	      "removing an app does not rewrite history");
	check(remembered.at(10) == 100,
	      "removed app remains remembered");
	active.insert(10);
	check(!FirstSeen::reconcile(remembered, active, 500),
	      "restoring an app preserves its original date");

	const FirstSeen::TimestampMap explicitTimestamps{{10, 999}, {30, 777}};
	const auto effective =
	    FirstSeen::effective(remembered, explicitTimestamps, active);
	check(effective.at(10) == 999,
	      "explicit timestamp overrides automatic first-seen");
	check(effective.at(20) == 200,
	      "automatic timestamp fills an active app without an override");
	check(effective.at(30) == 777,
	      "explicit timestamps for other apps remain supported");

	std::unordered_set<AppId_t> none;
	FirstSeen::TimestampMap empty;
	check(!FirstSeen::reconcile(empty, none, 600),
	      "empty discovery does not create state");
	check(!FirstSeen::reconcile(empty, {40}, 0),
	      "invalid zero clock fails closed");

	if (failures == 0)
		std::printf("all first-seen tests passed\n");
	return failures == 0 ? 0 : 1;
}
