/* ====== KAASHI BY LAXMI MEDHAVI — Application Logic ====== */

(function() {
  'use strict';

  // === State ===
  let products = [];
  let cart = [];
  let activeFilter = 'all';

  // === DOM References ===
  const productsGrid = document.getElementById('productsGrid');
  const cartBtn = document.getElementById('cartBtn');
  const cartCount = document.getElementById('cartCount');
  const cartSidebar = document.getElementById('cartSidebar');
  const cartOverlay = document.getElementById('cartOverlay');
  const cartClose = document.getElementById('cartClose');
  const cartItems = document.getElementById('cartItems');
  const cartEmpty = document.getElementById('cartEmpty');
  const cartFooter = document.getElementById('cartFooter');
  const cartTotal = document.getElementById('cartTotal');
  const placeOrderBtn = document.getElementById('placeOrderBtn');
  const shopNowBtn = document.getElementById('shopNowBtn');
  const copyUpiBtn = document.getElementById('copyUpi');
  const menuToggle = document.getElementById('menuToggle');
  const navLinks = document.getElementById('navLinks');
  const navbar = document.getElementById('navbar');

  // === Initialize ===
  document.addEventListener('DOMContentLoaded', init);

  function init() {
    fetchProducts();
    fetchCart();
    setupNavbar();
    setupScrollAnimations();
    setupFilters();
    setupCart();
    setupCopyUPI();
    setupMobileMenu();
    createParticles();
  }

  // === Products ===
  async function fetchProducts() {
    try {
      const res = await fetch('/api/products');
      products = await res.json();
      renderProducts(products);
    } catch (err) {
      console.error('Failed to fetch products:', err);
      productsGrid.innerHTML = '<p style="text-align:center;color:var(--color-text-dim);grid-column:1/-1;">Unable to load products. Please refresh the page.</p>';
    }
  }

  function renderProducts(items) {
    productsGrid.innerHTML = '';
    items.forEach((product, index) => {
      const card = document.createElement('div');
      card.className = 'product-card';
      card.style.animationDelay = `${index * 0.1}s`;

      const inCart = cart.find(c => c.productId === product.id);

      card.innerHTML = `
        <div class="product-img-wrap">
          <img src="${product.image}" alt="${product.name}" loading="lazy"
               onerror="this.parentElement.innerHTML='<div class=\\'product-img-placeholder ${getPlaceholderClass(product.name)}\\'><span>✦</span></div>'">
          <span class="product-badge">${product.weave}</span>
        </div>
        <div class="product-info">
          <p class="product-category">${product.category}</p>
          <h3 class="product-name">${product.name}</h3>
          <p class="product-desc">${product.description}</p>
          <div class="product-footer">
            <span class="product-price">₹${product.price.toLocaleString('en-IN')}</span>
            <button class="add-to-cart-btn ${inCart ? 'added' : ''}" data-id="${product.id}">
              ${inCart ? '✓ Added' : 'Add to Bag'}
            </button>
          </div>
        </div>
      `;

      // Add to cart click
      const addBtn = card.querySelector('.add-to-cart-btn');
      addBtn.addEventListener('click', () => addToCart(product.id, addBtn));

      productsGrid.appendChild(card);
    });
  }

  function getPlaceholderClass(name) {
    if (name.toLowerCase().includes('blue')) return 'blue';
    if (name.toLowerCase().includes('red') || name.toLowerCase().includes('bridal')) return 'red';
    if (name.toLowerCase().includes('peach')) return 'peach';
    return '';
  }

  // === Filters ===
  function setupFilters() {
    document.querySelectorAll('.filter-btn').forEach(btn => {
      btn.addEventListener('click', () => {
        document.querySelectorAll('.filter-btn').forEach(b => b.classList.remove('active'));
        btn.classList.add('active');
        activeFilter = btn.dataset.filter;

        if (activeFilter === 'all') {
          renderProducts(products);
        } else {
          const filtered = products.filter(p =>
            p.category.toLowerCase().includes(activeFilter.toLowerCase())
          );
          renderProducts(filtered);
        }
      });
    });
  }

  // === Cart ===
  async function fetchCart() {
    try {
      const res = await fetch('/api/cart');
      const data = await res.json();
      cart = data.items.map(item => ({ productId: item.productId, quantity: item.quantity }));
      updateCartUI(data);
    } catch (err) {
      console.error('Failed to fetch cart:', err);
    }
  }

  async function addToCart(productId, btnElement) {
    try {
      const res = await fetch('/api/cart', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ productId, quantity: 1 })
      });
      await res.json();

      btnElement.classList.add('added');
      btnElement.textContent = '✓ Added';

      showToast('Added to bag!');
      fetchCart();
    } catch (err) {
      console.error('Failed to add to cart:', err);
    }
  }

  async function removeFromCart(productId) {
    try {
      await fetch(`/api/cart/${productId}`, { method: 'DELETE' });
      fetchCart();
      // Re-render products to update button states
      if (activeFilter === 'all') {
        renderProducts(products);
      } else {
        const filtered = products.filter(p =>
          p.category.toLowerCase().includes(activeFilter.toLowerCase())
        );
        renderProducts(filtered);
      }
    } catch (err) {
      console.error('Failed to remove from cart:', err);
    }
  }

  async function updateQuantity(productId, quantity) {
    try {
      await fetch(`/api/cart/${productId}`, {
        method: 'PUT',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ quantity })
      });
      fetchCart();
    } catch (err) {
      console.error('Failed to update cart:', err);
    }
  }

  function updateCartUI(data) {
    const count = data.items.length;

    // Update count badge
    cartCount.textContent = count;
    cartCount.classList.toggle('active', count > 0);

    if (count === 0) {
      cartEmpty.style.display = 'flex';
      cartFooter.style.display = 'none';
      // Remove any old cart items
      cartItems.querySelectorAll('.cart-item').forEach(el => el.remove());
      return;
    }

    cartEmpty.style.display = 'none';
    cartFooter.style.display = 'block';
    cartTotal.textContent = `₹${data.total.toLocaleString('en-IN')}`;

    // Render cart items
    cartItems.querySelectorAll('.cart-item').forEach(el => el.remove());

    data.items.forEach(item => {
      if (!item.product) return;
      const el = document.createElement('div');
      el.className = 'cart-item';
      el.innerHTML = `
        <div class="cart-item-img">
          <img src="${item.product.image}" alt="${item.product.name}" 
               onerror="this.style.display='none'">
        </div>
        <div class="cart-item-details">
          <p class="cart-item-name">${item.product.name}</p>
          <p class="cart-item-price">₹${item.product.price.toLocaleString('en-IN')}</p>
          <div class="cart-item-controls">
            <button class="qty-btn qty-minus" data-id="${item.productId}">−</button>
            <span class="cart-item-qty">${item.quantity}</span>
            <button class="qty-btn qty-plus" data-id="${item.productId}">+</button>
            <button class="cart-item-remove" data-id="${item.productId}">Remove</button>
          </div>
        </div>
      `;

      el.querySelector('.qty-minus').addEventListener('click', () => {
        updateQuantity(item.productId, item.quantity - 1);
      });
      el.querySelector('.qty-plus').addEventListener('click', () => {
        updateQuantity(item.productId, item.quantity + 1);
      });
      el.querySelector('.cart-item-remove').addEventListener('click', () => {
        removeFromCart(item.productId);
      });

      cartItems.appendChild(el);
    });
  }

  function setupCart() {
    cartBtn.addEventListener('click', openCart);
    cartOverlay.addEventListener('click', closeCart);
    cartClose.addEventListener('click', closeCart);
    shopNowBtn.addEventListener('click', closeCart);

    placeOrderBtn.addEventListener('click', placeOrder);
  }

  function openCart() {
    cartSidebar.classList.add('active');
    cartOverlay.classList.add('active');
    document.body.style.overflow = 'hidden';
  }

  function closeCart() {
    cartSidebar.classList.remove('active');
    cartOverlay.classList.remove('active');
    document.body.style.overflow = '';
  }

  // === Order ===
  async function placeOrder() {
    const customerName = document.getElementById('customerName').value.trim();
    const customerPhone = document.getElementById('customerPhone').value.trim();
    const customerAddress = document.getElementById('customerAddress').value.trim();

    try {
      const res = await fetch('/api/cart');
      const cartData = await res.json();

      if (cartData.items.length === 0) {
        showToast('Your bag is empty!');
        return;
      }

      const orderItems = cartData.items.map(item => ({
        productId: item.productId,
        quantity: item.quantity
      }));

      const orderRes = await fetch('/api/orders', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          items: orderItems,
          customerName,
          customerPhone,
          customerAddress
        })
      });

      const orderData = await orderRes.json();

      // Open WhatsApp
      window.open(orderData.whatsappUrl, '_blank');

      // Clear cart
      await fetch('/api/cart', { method: 'DELETE' });
      fetchCart();
      closeCart();

      showToast('Order sent to WhatsApp!');

      // Re-render products to reset button states
      renderProducts(products);

    } catch (err) {
      console.error('Failed to place order:', err);
      showToast('Failed to place order. Please try again.');
    }
  }

  // === Copy UPI ===
  function setupCopyUPI() {
    if (copyUpiBtn) {
      copyUpiBtn.addEventListener('click', () => {
        const upiId = document.getElementById('upiId').textContent;
        navigator.clipboard.writeText(upiId).then(() => {
          showToast('UPI ID copied!');
        }).catch(() => {
          // Fallback
          const temp = document.createElement('textarea');
          temp.value = upiId;
          document.body.appendChild(temp);
          temp.select();
          document.execCommand('copy');
          document.body.removeChild(temp);
          showToast('UPI ID copied!');
        });
      });
    }
  }

  // === Navbar ===
  function setupNavbar() {
    window.addEventListener('scroll', () => {
      navbar.classList.toggle('scrolled', window.scrollY > 50);
    });
  }

  function setupMobileMenu() {
    menuToggle.addEventListener('click', () => {
      menuToggle.classList.toggle('active');
      navLinks.classList.toggle('active');
    });

    // Close menu on link click
    navLinks.querySelectorAll('.nav-link').forEach(link => {
      link.addEventListener('click', () => {
        menuToggle.classList.remove('active');
        navLinks.classList.remove('active');
      });
    });
  }

  // === Scroll Animations (Intersection Observer) ===
  function setupScrollAnimations() {
    const observer = new IntersectionObserver((entries) => {
      entries.forEach(entry => {
        if (entry.isIntersecting) {
          entry.target.classList.add('visible');
          observer.unobserve(entry.target);
        }
      });
    }, {
      threshold: 0.1,
      rootMargin: '0px 0px -50px 0px'
    });

    document.querySelectorAll('.animate-on-scroll').forEach(el => {
      observer.observe(el);
    });
  }

  // === Floating Particles (Hero) ===
  function createParticles() {
    const container = document.getElementById('heroParticles');
    if (!container) return;

    for (let i = 0; i < 20; i++) {
      const particle = document.createElement('div');
      particle.style.cssText = `
        position: absolute;
        width: ${Math.random() * 4 + 1}px;
        height: ${Math.random() * 4 + 1}px;
        background: rgba(201, 168, 76, ${Math.random() * 0.3 + 0.05});
        border-radius: 50%;
        top: ${Math.random() * 100}%;
        left: ${Math.random() * 100}%;
        animation: float ${Math.random() * 4 + 4}s ease-in-out infinite;
        animation-delay: ${Math.random() * 4}s;
      `;
      container.appendChild(particle);
    }
  }

  // === Toast Notification ===
  function showToast(message) {
    const existing = document.querySelector('.toast');
    if (existing) existing.remove();

    const toast = document.createElement('div');
    toast.className = 'toast';
    toast.textContent = message;
    document.body.appendChild(toast);

    requestAnimationFrame(() => {
      toast.classList.add('show');
    });

    setTimeout(() => {
      toast.classList.remove('show');
      setTimeout(() => toast.remove(), 400);
    }, 2500);
  }

})();
