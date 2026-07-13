FROM alpine:3.19
RUN apk add --no-cache gcc musl-dev openssl-dev openssl-libs-static
WORKDIR /app
COPY fivebit_webrtc.c fivebit_grid.c fivebit_grid.h ./
RUN gcc -O2 -static -o fivebit_webrtc fivebit_webrtc.c fivebit_grid.c -lssl -lcrypto && strip fivebit_webrtc
EXPOSE 8085
RUN mkdir -p /data
CMD ./fivebit_webrtc "$PORT" /data
