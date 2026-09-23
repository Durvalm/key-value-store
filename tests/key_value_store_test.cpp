#include "key_value_store.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace
{
int failures = 0;

void expect(bool condition, const std::string& message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void test_new_store_is_empty()
{
    KeyValueStore store;

    expect(store.size() == 0, "a new store has size zero");
    expect(!store.get("missing").has_value(), "a missing key returns nullopt");
}

void test_set_and_get()
{
    KeyValueStore store;

    store.set("name", "Durval");

    const auto value = store.get("name");
    expect(value.has_value(), "get finds a stored key");
    expect(value.value_or("") == "Durval", "get returns the stored value");
    expect(store.size() == 1, "set increases the store size");
}

void test_set_overwrites_existing_value()
{
    KeyValueStore store;
    store.set("language", "Python");
    store.set("language", "C++");

    expect(store.get("language").value_or("") == "C++", "set overwrites a value");
    expect(store.size() == 1, "overwriting does not add another key");
}

void test_remove_existing_key()
{
    KeyValueStore store;
    store.set("temporary", "value");

    expect(store.remove("temporary"), "remove reports an existing key");
    expect(!store.get("temporary").has_value(), "removed keys cannot be retrieved");
    expect(store.size() == 0, "remove decreases the store size");
}

void test_remove_missing_key()
{
    KeyValueStore store;

    expect(!store.remove("missing"), "remove reports a missing key");
    expect(store.size() == 0, "removing a missing key does not change the size");
}

void test_keys_are_independent()
{
    KeyValueStore store;
    store.set("first", "one");
    store.set("second", "two");
    store.remove("first");

    expect(!store.get("first").has_value(), "the selected key is removed");
    expect(store.get("second").value_or("") == "two", "other keys remain stored");
    expect(store.size() == 1, "only one key remains");
}
} // namespace

int main()
{
    test_new_store_is_empty();
    test_set_and_get();
    test_set_overwrites_existing_value();
    test_remove_existing_key();
    test_remove_missing_key();
    test_keys_are_independent();

    if (failures != 0)
    {
        std::cerr << failures << " test assertion(s) failed.\n";
        return EXIT_FAILURE;
    }

    std::cout << "All key-value store tests passed.\n";
    return EXIT_SUCCESS;
}
