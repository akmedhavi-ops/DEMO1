const express = require('express');
const router = express.Router();
const path = require('path');
const fs = require('fs');

// In-memory cart store
let cart = [];

// Load products for reference
const productsPath = path.join(__dirname, '..', 'data', 'products.json');

function getProducts() {
  const data = fs.readFileSync(productsPath, 'utf8');
  return JSON.parse(data);
}

// GET /api/cart — get cart with product details
router.get('/', (req, res) => {
  const products = getProducts();
  const cartWithDetails = cart.map(item => {
    const product = products.find(p => p.id === item.productId);
    return {
      ...item,
      product: product || null
    };
  });
  
  const total = cartWithDetails.reduce((sum, item) => {
    return sum + (item.product ? item.product.price * item.quantity : 0);
  }, 0);
  
  res.json({ items: cartWithDetails, total, count: cart.length });
});

// POST /api/cart — add item to cart
router.post('/', (req, res) => {
  const { productId, quantity = 1 } = req.body;
  
  if (!productId) {
    return res.status(400).json({ error: 'productId is required' });
  }
  
  const products = getProducts();
  const productExists = products.find(p => p.id === productId);
  if (!productExists) {
    return res.status(404).json({ error: 'Product not found' });
  }
  
  const existingItem = cart.find(item => item.productId === productId);
  if (existingItem) {
    existingItem.quantity += quantity;
  } else {
    cart.push({ productId, quantity });
  }
  
  res.json({ message: 'Item added to cart', cart });
});

// PUT /api/cart/:productId — update quantity
router.put('/:productId', (req, res) => {
  const productId = parseInt(req.params.productId);
  const { quantity } = req.body;
  
  const item = cart.find(i => i.productId === productId);
  if (!item) {
    return res.status(404).json({ error: 'Item not in cart' });
  }
  
  if (quantity <= 0) {
    cart = cart.filter(i => i.productId !== productId);
  } else {
    item.quantity = quantity;
  }
  
  res.json({ message: 'Cart updated', cart });
});

// DELETE /api/cart/:productId — remove item
router.delete('/:productId', (req, res) => {
  const productId = parseInt(req.params.productId);
  cart = cart.filter(item => item.productId !== productId);
  res.json({ message: 'Item removed', cart });
});

// DELETE /api/cart — clear cart
router.delete('/', (req, res) => {
  cart = [];
  res.json({ message: 'Cart cleared' });
});

module.exports = router;
