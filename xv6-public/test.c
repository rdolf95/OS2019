#include "types.h"
#include "stat.h"
#include "user.h"
#include "fs.h"
#include "fcntl.h"

#define BLOCK_NUM (2 * 1024 * 1)
#define BLOCK_SIZE (256)
#define STRESS_NUM (4)

int
main(int argc, char *argv[])
{
  int fd, i, j; 
  int r;
  //int total;
  char *path = (argc > 1) ? argv[1] : "syncfile";
  char data[BLOCK_SIZE];
  //char buf[BLOCK_SIZE];

  printf(1, "synctest starting\n");
  const int sz = sizeof(data);
  for (i = 0; i < sz; i++) {
      data[i] = '1';//i % 128;
  }

// ============================================================================
// ============================================================================

  printf(1, "write and not sync test\n");
  fd = open(path, O_CREATE | O_RDWR);
  if ((r = write(fd, data, sizeof(data))) != sizeof(data)){
    printf(1, "write returned %d : failed\n", r);
    exit();
  }
  sync();
  printf(1, "write and sync done\n");

  
  printf(1, "stress test for sync\n");
  for(j = 0; j<26; j++){
    for (i = 0; i < sz; i++) {
      data[i] = 'a' + j;
    }
    if ((r = write(fd, data, sizeof(data))) != sizeof(data)){
      printf(1, "write returned %d : failed\n", r);
      exit();
    }
  }
  //sync();
  printf(1, "write and sync done\n");
  i = get_log_num();
  printf(1, "log_num : %d\n", i);
  
  for(i = 10; i>=0; i--){
    printf(1, "FILE will be closed in %d\n",i);
    sleep(30);
  }
  close(fd);

  exit();
}

