SELECT p.name, SUM(oi.quantity) AS total_quantity
FROM products p
JOIN order_items oi ON p.id = oi.product_id
GROUP BY p.id, p.name
HAVING SUM(oi.quantity) > 1;