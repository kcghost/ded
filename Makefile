.PHONY: check test install

check:
	shellcheck ded.sh

test:
	./test.sh

install:
	install -Dm755 ded.sh /usr/bin/ded
