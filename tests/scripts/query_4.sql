SELECT u.first_name, u.last_name, COUNT(*) AS number_of_orders
FROM users u
JOIN orders o ON u.id = o.user_id
WHERE o.order_date BETWEEN '2023-01-01' AND '2023-12-31'
GROUP BY u.id, u.first_name, u.last_name;