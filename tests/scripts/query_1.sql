SELECT u.first_name, u.last_name, o.order_date, o.total_amount, o.status
FROM users u
JOIN orders o ON u.id = o.user_id
WHERE o.total_amount > 70000.00;