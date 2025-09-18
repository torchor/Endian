### C++  std::promise 和 std::future 使用的注意事项

- promise 和  future 都是不可复制，可移动。对空的对象操作会抛出异常。
- future调用get时，对应的promise还没有set就析构了，会抛出异常。
- std::async(std::launch::deferred 参数，不会创建新的线程，在调用future.get所在的线程执行。且返回的future对象析构时，不会自动阻塞等待
- std::async(std::launch::async 参数，会创建新的线程