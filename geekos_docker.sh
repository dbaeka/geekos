#!/usr/bin/env bash

IMAGE_NAME=geekos
CONTAINER_NAME=geekos

# in order to fully enable x-forwarding, run `xhost +local:` ref: https://wiki.archlinux.org/index.php/Xhost
# note that the qemu window does not scale with resolution so it may show up tiny on high-resolution displays

# if image doesn't exist, build it
IMAGE=$(docker images --filter reference=${IMAGE_NAME} --format "{{.Repository}}")
if [ "$IMAGE" != "$IMAGE_NAME" ]; then
    echo "Building geekos image"
    docker build -t ${IMAGE_NAME} .
fi

# if container doesn't exist, create it
CONTAINER=$(docker container ls -a --filter name=${CONTAINER_NAME} --format "{{.Names}}")
if [ "$CONTAINER" != "$CONTAINER_NAME" ]; then
    echo "Creating new container"
    docker create -it \
    -v $(pwd):/geekos \
    -w /geekos \
    -u $(id -u) \
    -p 12345:12345 \
    -e DISPLAY=$DISPLAY \
    -v /etc/group:/etc/group:ro \
    -v /etc/passwd:/etc/passwd:ro \
    -v /etc/shadow:/etc/shadow:ro \
    -v /etc/sudoers:/etc/sudoers:ro \
    -v /tmp/.X11-unix:/tmp/.X11-unix:rw \
    --name ${CONTAINER_NAME} ${IMAGE_NAME}
fi

# if container isn't running, start it
STATUS=$(docker container ls --filter name=${CONTAINER_NAME} --format "{{.Names}}")
if [ "$STATUS" != "$CONTAINER_NAME" ]; then
    echo "Spinning up $CONTAINER_NAME"
    docker start -ai ${CONTAINER_NAME}
else
    echo "Connecting to new terminal"
    docker exec -it ${CONTAINER_NAME} bash
fi
