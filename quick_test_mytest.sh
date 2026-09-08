  # export LD_LIBRARY_PATH=~/.conda/envs/pod5-deps/lib:${LD_LIBRARY_PATH:-}
  # 分别跑
  cd ./build/c++/test
  # ./build/c++/test/pod5_unit_tests "[mytest1]"
  # ./build/c++/test/pod5_unit_tests "[mytest2]"
  # ./pod5_unit_tests "[mytest3]"
  ./pod5_unit_tests "[mytest4]"
  # 三个一起跑
  # ./build/c++/test/pod5_unit_tests "[mytest1],[mytest2],[mytest3]"

