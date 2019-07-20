#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <err.h>

int main () {
   //char str1[20];
   //char str2[20];
   //int result;
  
  if (pledge("error", NULL) == -1) 
    err(1, "pledge"); 

   //abort();
   ////Assigning the value to the string str1
   //strcpy(str1, "hello");

   ////Assigning the value to the string str2
   //strcpy(str2, "hEllo");
   return 1;
}
