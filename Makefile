.PHONY: check test install clean

all: gpt

check:
	shellcheck ded.sh
	shellcheck gpt.sh

test:
	./test.sh

install: gpt
	install -Dm755 gpt /usr/local/bin/gpt
	install -Dm755 ded.sh /usr/local/bin/ded
	install -Dm644 gpt.ids /usr/local/share/misc/gpt.ids

clean:
	rm -f gpt
