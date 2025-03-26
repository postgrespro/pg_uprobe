FROM postgres:16

RUN apt-get update && \
    apt-get install -y \
    build-essential \
    postgresql-server-dev-16 \
    git \
    gcc \
    python3 \
    libkrb5-dev \
    krb5-multidev \
    cmake \
    && rm -rf /var/lib/apt/lists/*

COPY ./pg_uprobe/ /usr/src/pg_uprobe

WORKDIR /usr/src/pg_uprobe
RUN make USE_PGXS=1 PG_CONFIG=pg_config install

COPY ./configure.sh /docker-entrypoint-initdb.d/


EXPOSE 5432
CMD ["postgres"]
