CREATE TABLE users (
    id SERIAL PRIMARY KEY,
    first_name VARCHAR(100) NOT NULL,
    last_name VARCHAR(100),
    email VARCHAR(255) UNIQUE NOT NULL,
    phone_number VARCHAR(20),
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE products (
    id SERIAL PRIMARY KEY,
    name VARCHAR(200) NOT NULL,
    description TEXT,
    price DECIMAL(10, 2) NOT NULL CHECK(price >= 0),
    stock INT DEFAULT 0 CHECK(stock >= 0)
);

CREATE TABLE orders (
    id SERIAL PRIMARY KEY,
    user_id INT REFERENCES users(id) ON DELETE CASCADE,
    order_date TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    total_amount DECIMAL(10, 2) NOT NULL CHECK(total_amount >= 0),
    status VARCHAR(50) DEFAULT 'NEW' CHECK(status IN ('NEW', 'PROCESSING', 'SHIPPED', 'DELIVERED', 'CANCELLED'))
);

CREATE TABLE order_items (
    id SERIAL PRIMARY KEY,
    order_id INT REFERENCES orders(id) ON DELETE CASCADE,
    product_id INT REFERENCES products(id) ON DELETE RESTRICT,
    quantity INT DEFAULT 1 CHECK(quantity > 0),
    unit_price DECIMAL(10, 2) NOT NULL CHECK(unit_price >= 0)
);



CREATE OR REPLACE FUNCTION calculate_order_total(order_id_2 INT)
RETURNS DECIMAL(10, 2) AS $$
DECLARE
    total DECIMAL(10, 2);
BEGIN
    SELECT SUM(quantity * unit_price) INTO total
    FROM order_items
    WHERE order_items.order_id = order_id_2;

    RETURN COALESCE(total, 0);
END;
$$ LANGUAGE plpgsql;


CREATE OR REPLACE FUNCTION update_order_status(
    p_order_id INT,
    p_new_status VARCHAR(50))
RETURNS VOID AS $$
BEGIN
    -- Validate status
    IF p_new_status NOT IN ('NEW', 'PROCESSING', 'SHIPPED', 'DELIVERED', 'CANCELLED') THEN
        RAISE EXCEPTION 'Invalid order status: %', p_new_status;
    END IF;

    UPDATE orders
    SET status = p_new_status
    WHERE id = p_order_id;
END;
$$ LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION add_product_to_order(
    p_order_id INT,
    p_product_id INT,
    p_quantity INT)
RETURNS VOID AS $$
DECLARE
    v_price DECIMAL(10, 2);
    v_stock INT;
BEGIN

    SELECT price, stock INTO v_price, v_stock
    FROM products
    WHERE id = p_product_id;

    IF v_stock < p_quantity THEN
        RAISE EXCEPTION 'Insufficient stock for product ID % (available: %, requested: %)',
            p_product_id, v_stock, p_quantity;
    END IF;


    INSERT INTO order_items (order_id, product_id, quantity, unit_price)
    VALUES (p_order_id, p_product_id, p_quantity, v_price);


    UPDATE products
    SET stock = stock - p_quantity
    WHERE id = p_product_id;


    UPDATE orders
    SET total_amount = total_amount + (v_price * p_quantity)
    WHERE id = p_order_id;
END;
$$ LANGUAGE plpgsql;


CREATE OR REPLACE FUNCTION get_user_orders(p_user_id INT)
RETURNS TABLE (
    order_id INT,
    total_amount DECIMAL(10, 2),
    status VARCHAR(50),
    item_count BIGINT
) AS $$
BEGIN
    RETURN QUERY
    SELECT
        o.id AS order_id,
        o.total_amount,
        o.status,
        COUNT(oi.id)::BIGINT AS item_count
    FROM orders o
    LEFT JOIN order_items oi ON o.id = oi.order_id
    WHERE o.user_id = p_user_id
    GROUP BY o.id;
END;
$$ LANGUAGE plpgsql;


CREATE OR REPLACE FUNCTION create_user(
    p_first_name VARCHAR(100),
    p_last_name VARCHAR(100),
    p_email VARCHAR(255),
    p_phone_number VARCHAR(20) DEFAULT NULL)
RETURNS INT AS $$
DECLARE
    v_user_id INT;
BEGIN

    IF p_email !~ '^[A-Za-z0-9._%-]+@[A-Za-z0-9.-]+[.][A-Za-z]+$' THEN
        RAISE EXCEPTION 'Invalid email format';
    END IF;


    INSERT INTO users (first_name, last_name, email, phone_number)
    VALUES (p_first_name, p_last_name, p_email, p_phone_number)
    RETURNING id INTO v_user_id;

    RETURN v_user_id;
EXCEPTION
    WHEN unique_violation THEN
        RAISE EXCEPTION 'Email % already exists', p_email;
END;
$$ LANGUAGE plpgsql;


CREATE OR REPLACE FUNCTION get_low_stock_products(p_threshold INT DEFAULT 5)
RETURNS TABLE (
    product_id INT,
    product_name VARCHAR(200),
    current_stock INT,
    product_price DECIMAL(10, 2)
) AS $$
BEGIN
    RETURN QUERY
    SELECT
        id AS product_id,
        name AS product_name,
        stock AS current_stock,
        price AS product_price
    FROM products
    WHERE stock < p_threshold
    ORDER BY stock ASC;
END;
$$ LANGUAGE plpgsql;



CREATE OR REPLACE FUNCTION validate_user_exists(p_user_id INT)
RETURNS BOOLEAN AS $$
DECLARE
    user_count INT;
BEGIN
    SELECT COUNT(*) INTO user_count FROM users WHERE id = p_user_id;
    RETURN user_count > 0;
END;
$$ LANGUAGE plpgsql;


CREATE OR REPLACE FUNCTION create_new_order(p_user_id INT)
RETURNS INT AS $$
DECLARE
    new_order_id INT;
BEGIN
    IF NOT validate_user_exists(p_user_id) THEN
        RAISE EXCEPTION 'User with ID % does not exist', p_user_id;
    END IF;

    INSERT INTO orders (user_id, total_amount)
    VALUES (p_user_id, 0)
    RETURNING id INTO new_order_id;

    RETURN new_order_id;
END;
$$ LANGUAGE plpgsql;



CREATE OR REPLACE FUNCTION process_complete_order(
    p_user_id INT,
    p_product_ids INT[],
    p_quantities INT[]
) RETURNS INT AS $$
DECLARE
    v_order_id INT;
    v_product_id INT;
    v_quantity INT;
    i INT;
BEGIN

    IF array_length(p_product_ids, 1) != array_length(p_quantities, 1) THEN
        RAISE EXCEPTION 'Product IDs and quantities arrays must have the same length';
    END IF;

    v_order_id := create_new_order(p_user_id);

    FOR i IN 1..array_length(p_product_ids, 1) LOOP
        v_product_id := p_product_ids[i];
        v_quantity := p_quantities[i];

        PERFORM add_product_to_order(
            v_order_id,
            v_product_id,
            v_quantity
        );
    END LOOP;

    UPDATE orders
    SET total_amount = calculate_order_total(v_order_id)
    WHERE id = v_order_id;

    RETURN v_order_id;
EXCEPTION
    WHEN OTHERS THEN

        IF v_order_id IS NOT NULL THEN
            PERFORM update_order_status(v_order_id, 'CANCELLED');
        END IF;
        RAISE;
END;
$$ LANGUAGE plpgsql;