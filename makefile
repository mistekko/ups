PREFIX ?= "/usr/local"
CC ?= cc
CFLAGS += -ggdb -std=c99 -Wall -Wextra -pedantic
up_html_size = $(shell du -b up.html | cut -f1)
favicon_size = $(shell du -b favicon.svg | cut -f1)

status_template = 'char %s[] = "HTTP/1.1 200 OK\\r\\n"\n'
length_template = '"Content-Length: %s\\r\\n"\n'
type_template = '"Content-Type: %s\\r\\n"\n'
sed_sanitize_script = \
	-e 's/\\/\\\\/g' \
	-e 's/"/\\"/g' \
	-e 's/^/"/g' \
	-e 's/$$/\\n"/g' \

up: up.c resources.h
	${CC} ${CFLAGS} -o up up.c

resources.h: up.html favicon.svg makefile
# ensure that sizeof(up.html) is four digits
	[ ${up_html_size} -lt 10000 ] || exit 1
	[ ${up_html_size} -gt   999 ] || exit 1
# ensure that sizeof(favicon.svg) is three digits
	[ ${favicon_size} -lt 1000 ] || exit 1
	[ ${favicon_size} -gt   99 ] || exit 1
	@echo

	printf "" > resources.h
	@echo

	printf ${status_template} document_packet >> resources.h
	printf ${length_template} ${up_html_size} >> resources.h
	printf ${type_template}   "text/html; charset=UTF-8" >> resources.h
	printf '"\\r\\n"\n' >> resources.h
	sed ${sed_sanitize_script} up.html >> resources.h
	echo ";" >> resources.h
	@echo

	printf ${status_template} favicon_packet >> resources.h
	printf ${length_template} ${favicon_size} >> resources.h
	printf ${type_template}   "image/svg+xml" >> resources.h
	printf '"\\r\\n"\n' >> resources.h
	sed ${sed_sanitize_script} favicon.svg >> resources.h
	echo ";" >> resources.h
	@echo # spacer for any subsequent recipes

tester: up tester.c
	${CC} ${CFLAGS} -o tester tester.c

all: tester up

install: up
	cp -f up "${PREFIX}/bin"

clean:
	rm resources.h up tester test 2>/dev/null # `test' is created during testing
