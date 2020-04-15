# Dockerfile for obbaa-polt-simulator

FROM ubuntu:latest

# install OS and perl packages
# XXX just perl is probably sufficient/better?
RUN apt-get update \
 && apt-get --yes install \
            automake \
            ccache \
            cmake \
            cpanminus \
            g++ \
            gcc \
            libtool \
            m4 \
            make \
            patch \
            pkg-config \
            wget \
            xz-utils \
            zip \
 && cpanm \
            FindBin \
 && apt-get clean

# set working directory
WORKDIR /opt/obbaa-polt-simulator

# copy pOLT simulator code
COPY . .

# build pOLT simulator (this installs to build/fs)
# XXX probably don't need/want these make variables:
#     CCACHE=n OPENSSL_VERSION=1.1.1
RUN make \
      CCACHE=n

# XXX should now create some /usr/local/bin etc. links; or move build/fs
#     subdirs to /usr/local?
