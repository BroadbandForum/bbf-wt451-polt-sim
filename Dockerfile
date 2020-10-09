# Dockerfile for obbaa-polt-simulator
ARG FROM_TAG=latest

# image to build the code
FROM ubuntu:$FROM_TAG AS builder

# install OS and perl packages
RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get --yes install \
            automake \
            ccache \
            cmake \
            cpanminus \
            g++ \
            gcc \
            git \
            libtool \
            m4 \
            make \
            patch \
            pkg-config \
            wget \
            xz-utils \
            zip \
            python3 \
            vim \
            openssh-client \
 && cpanm \
            FindBin \
 && apt-get clean

# set working directory
WORKDIR /opt/obbaa-polt-simulator

# copy pOLT simulator code
COPY . .

# build pOLT simulator (this installs to build/fs)
RUN make


# image to run the code
FROM ubuntu:$FROM_TAG

ARG PASSWD=root
ARG DEBUG

RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get --yes install \
            openssh-client \
            openssl \
&& if [ -n "$DEBUG" ] ; \
    then DEBIAN_FRONTEND=noninteractive apt-get --yes install \
            vim \
            gdb \
            wget \
            net-tools \
            iputils-ping \
            python3 \
            python3-pip \
            tshark \
            valgrind ; \
    else : ; fi \
 && apt-get clean

# copy build/fs subdirs to /usr/local
COPY --from=builder /opt/obbaa-polt-simulator/build/fs/ /usr/local/
COPY certificates/ certificates/
RUN mkdir -p opt/obbaa-polt-simulator/build/fs && cp -Rsf /usr/local/* /opt/obbaa-polt-simulator/build/fs/ && ldconfig && echo root:$PASSWD | chpasswd

# generate RSA key pair
RUN mkdir -p /root/.ssh
RUN openssl genrsa -out /root/.ssh/polt.pem
RUN openssl req -new -x509 -sha256 -key /root/.ssh/polt.pem -out /root/.ssh/polt.cer -days 3650 -config certificates/request.conf

# copy the simulator's certificate and start netopeer2
ENTRYPOINT ["/usr/local/start_netconf_server.sh", "-d"]
