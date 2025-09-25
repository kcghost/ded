.PHONY: check test install clean

all: gpt

check:
	shellcheck ded.sh
	shellcheck gpt.sh

test:
	./test.sh

install: gpt
	install -Dm755 gpt /usr/bin/gpt
	install -Dm755 ded.sh /usr/bin/ded

clean:
	rm -f gpt
