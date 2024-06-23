A header only thread pool targeting C++20

## Getting Started
Clone the repo
```[bash]
git clone git@github.com:haciMMicah/pool_party.git && cd pool_party
```
Build the tests
```[bash]
mkdir build && cd build && cmake ../ && cmake --build .
```
Run the tests
```
./pool_party_tests
```
## Usage
### Thread Pool Creation
```cpp
// Create a thread pool with std::thread::hardware_concurrency() number of threads
pool_party::thread_pool pool{};
// Create a thread pool with a given number of threads
pool_party::thread_pool pool{8};
```
### Starting and Stoping the Thread Pool
```cpp
pool_party::thread_pool pool{};
pool.start();
pool.stop();
```
### Get Information about the Thread Pool
```cpp
pool.num_threads();
pool.is_running();
```
## Submitting Jobs to the Thread Pool
```cpp
pool_party::thread_pool pool{};
pool.start();

// Submit a detached job
pool.submit_detached([]{
  std::cout << "Hello, World!\n";
})

// Submit a waitable job via futures
auto future = pool.submit([]{
  std::cout << "Hello, World!\n";
})
future.wait();

// Submit a job with a callback to signal its completion
pool.submit_with_callback(
  []{ std::cout << "Hello, "; },
  []{ std::cout << "World!\n"; });

// Submit a job that passes its return value to a callback
pool.submit_with_callback(
  []{ std::cout << "Hello, "; return 42; },
  [](int num){ std::cout << "World! " << num << "\n"; });

// Submit a job that takes arguments that passes its return value to a callback
pool.submit_with_callback(
  [](int num_to_return){ std::cout << "Hello, "; return num_to_return; },
  [](int num){ std::cout << "World! " << num << "\n"; },
  42);
```
