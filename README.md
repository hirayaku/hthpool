# hthpool
hirayaku-threadpool (hthpool), a pure C thread pool implementation
**hthpool** is implemented with a concurrent bounded queue (`worklist.c`) and upper-layer wrapper. There are two types of threads: the main thread which is responsible for managing the threadpool, and worker threads in the threadpool.
- `int hthpool_init(int num)`: initialize a fixed-size threadpool with size `num`. **Only allowed to be called by the main thread**.
- `int hthpool_submit(task item)`: submit new tasks to the threadpool. Tasks are executed in FIFO order. See `common.h` for `task` definition.
- `void hthpool_stop(void)`: stop the execution of tasks in the worklist and make all worker threads in a pending state. Threadpool enters into inactive state.
- `void hthpool_wait(void)`: wait until all worker threads are stopped (pending state). **Only allowed to be called by the main thread**.
- `void hthpool_continue(void)`: make threadpool active again. All previous tasks in the worklist are thrown. Must be called after `hthpool_wait`. **Only allowed to be called by the main thread**.
- `void hthpool_destroy(void)`: destroy the threadpool. Must be called after `hthpool_wait`. **Only allowed to be called by the main thread**.
