# sdkdemotest
sdkdemotest

## UDP Echo Client / Server

A simple C++ UDP echo server and client located in the `udp/` directory.

### Build

```bash
cd udp
make
```

### Run

Start the server (default port 8888):

```bash
./udp_server          # listens on port 8888
./udp_server 9999     # listens on port 9999
```

In another terminal, start the client:

```bash
./udp_client                    # connects to 127.0.0.1:8888
./udp_client 127.0.0.1 9999    # connects to 127.0.0.1:9999
```

Type a message and press Enter. The server echoes it back.
