.PHONY: check test install

all: gpt

check:
	shellcheck ded.sh
	shellcheck gpt.sh

test:
	./test.sh

install:
	install -Dm755 ded.sh /usr/bin/ded
