all:
	${CC} -O2 -march=native -g -Wall -Wextra -o simple_lb simple_lb.c

debug:
	${CC} -O0 -g -Wall -Wextra -fno-omit-frame-pointer -fsanitize=address,leak,undefined -o simple_lb simple_lb.c

debug2:
	${CC} -O0 -g -Wall -Wextra -fno-omit-frame-pointer -fsanitize=undefined,memory -o simple_lb simple_lb.c
