# UDP File Transfer over SSH

A file transfer tool that sends UDP datagrams through an existing SSH connection, without requiring router or firewall changes. It implements the BBR algorithm for DIL networks to ensure minimal overhead in the cases of latency or packet loss.

This program is a purely CLI application.

## Dependencies
- udp-over-tcp (https://github.com/mullvad/udp-over-tcp) installed on both machines
- OpenSSH client and server

## Build
In the root project directory, run the following:
```
    mkdir build && cd build
    cmake ..
    cmake --build .
```

## Run

On the local machine:

```
./scripts/setup_tunnel.sh user@remote-host
```

or alternately you can run it directly without the script via:
```
./udp_transfer send <ip> <port> <file_to_send>
```

Then in another terminal (or on a remote machine):

```
./build/udp_transfer receive
```

Enter 127.0.0.1 as the target IP if this is going to be run locally, or the IP of the remote host if it's to be run across a network. By default, it uses port 3000 for the transfe.r The datagrams will be tunneled through SSH to the remote machine, where they emerge as UDP on port 6000.

## Command-line arguments
There is one other command-line argument to have the program run it's unit tests on the internal wiring. It's not really user facing, but to run them, issue the following command: `./udp_transfer --run-unit-tests`

## Optional Configuration
This application assumes that OpenSSH is being used as the SSHd server.

## Minimal Example
To transfer a file between 2 computers, this is a minimal working example. There is also a script in the scripts/ folder that can help expedite this once a user is comfortable with the command-line example.

Remote machine, terminal 1:
`./udp_transfer receive`

Remote machine, terminal 2:
`tcp2udp --tcp-listen 0.0.0.0:4000 --udp-forward 127.0.0.1:3000`

Local machine, terminal 1:
`ssh -N -L 5000:localhost:4000 user@remote-host`

Local machine, terminal 2:
`udp2tcp --udp-listen 127.0.0.1:3000 --tcp-forward 127.0.0.1:5000`

Local machine, terminal 3:
`./udp_transfer send 127.0.0.1 3000 test.txt`