FROM docker:19.03.2 as runtime

RUN apk update \
  && apk upgrade \
  && apk add --no-cache git

COPY /* /
ENTRYPOINT ["/buildall.sh"]

