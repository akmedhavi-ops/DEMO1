const express = require('express');
const router = express.Router();
const path = require('path');
const fs = require('fs');

const WHATSAPP_NUMBER = '919407164023';
const UPI_ID = 'laxmimedhavi@oksbi';

const productsPath = path.join(__dirname, '..', 'data', 'products.json');

function getProducts() {
  const data = fs.readFileSync(productsPath, 'utf8');
  return JSON.parse(data);
}

// POST /api/orders — place an order
router.post('/', (req, res) => {
  const { items, customerName, customerPhone, customerAddress } = req.body;
  
  if (!items || items.length === 0) {
    return res.status(400).json({ error: 'Order must contain at least one item' });
  }
  
  const products = getProducts();
  
  let orderSummary = '🙏 *KAASHI BY LAXMI MEDHAVI*\n';
  orderSummary += '━━━━━━━━━━━━━━━━━━\n';
  orderSummary += '*New Order*\n\n';
  
  if (customerName) orderSummary += `*Name:* ${customerName}\n`;
  if (customerPhone) orderSummary += `*Phone:* ${customerPhone}\n`;
  if (customerAddress) orderSummary += `*Delivery Address:* ${customerAddress}\n`;
  orderSummary += '\n';
  
  let total = 0;
  
  items.forEach((item, index) => {
    const product = products.find(p => p.id === item.productId);
    if (product) {
      const subtotal = product.price * item.quantity;
      total += subtotal;
      orderSummary += `${index + 1}. *${product.name}*\n`;
      orderSummary += `   Qty: ${item.quantity} × ₹${product.price.toLocaleString('en-IN')}\n`;
      orderSummary += `   Subtotal: ₹${subtotal.toLocaleString('en-IN')}\n\n`;
    }
  });
  
  orderSummary += '━━━━━━━━━━━━━━━━━━\n';
  orderSummary += `*Total: ₹${total.toLocaleString('en-IN')}*\n\n`;
  orderSummary += `💳 *UPI Payment:* ${UPI_ID}\n`;
  orderSummary += '\nPlease confirm my order. 🙏';
  
  const whatsappUrl = `https://wa.me/${WHATSAPP_NUMBER}?text=${encodeURIComponent(orderSummary)}`;
  
  res.json({
    message: 'Order prepared',
    orderId: `KBL-${Date.now()}`,
    total,
    whatsappUrl,
    upiId: UPI_ID
  });
});

module.exports = router;
