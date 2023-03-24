FROM debian

# https://docs.docker.com/develop/develop-images/dockerfile_best-practices/#run
run apt-get update && apt-get install -y build-essential git cmake \
 && rm -rf /var/lib/apt/lists/*

