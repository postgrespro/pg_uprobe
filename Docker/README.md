git clone git@github.com:postgrespro/pg_uprobe.git

docker build -t pg16-pg_uprobe:latest .

docker run -d \
  --name pg16-pg_uprobe \
  -e POSTGRES_USER=postgres \
  -e POSTGRES_PASSWORD=postgres \
  -e POSTGRES_DB=postgres \
  -p 5432:5432 \
  -v ./pg16-data:/var/lib/postgresql/data \
  pg16-pg_uprobe:latest


docker exec -it pg16-pg_uprobe sh