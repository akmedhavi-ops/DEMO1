const express = require('express');
const router = express.Router();
const path = require('path');
const fs = require('fs');

// Load products from JSON
const productsPath = path.join(__dirname, '..', 'data', 'products.json');

router.get('/', (req, res) => {
  try {
    const data = fs.readFileSync(productsPath, 'utf8');
    const products = JSON.parse(data);
    
    // Optional category filter
    const { category } = req.query;
    if (category) {
      const filtered = products.filter(p => 
        p.category.toLowerCase().includes(category.toLowerCase())
      );
      return res.json(filtered);
    }
    
    res.json(products);
  } catch (err) {
    res.status(500).json({ error: 'Failed to load products' });
  }
});

router.get('/:id', (req, res) => {
  try {
    const data = fs.readFileSync(productsPath, 'utf8');
    const products = JSON.parse(data);
    const product = products.find(p => p.id === parseInt(req.params.id));
    
    if (!product) {
      return res.status(404).json({ error: 'Product not found' });
    }
    
    res.json(product);
  } catch (err) {
    res.status(500).json({ error: 'Failed to load product' });
  }
});

module.exports = router;
