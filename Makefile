all:
	${CC} -O2 -g -Wall -Wextra -o simple_lb simple_lb.c

debug:
	${CC} -O0 -g -Wall -Wextra -fsanitize=address -o simple_lb simple_lb.c
