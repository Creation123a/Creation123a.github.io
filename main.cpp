// ============================================================================
//  Aurora Coffee Co. — Booking System Backend
//  OOP C++ Microproject
//
//  Build:
//    Windows (MinGW):  g++ main.cpp -o server.exe -lws2_32 -lwsock32 -std=c++17 -O2
//    Linux / macOS:    g++ main.cpp -o server -lpthread -std=c++17 -O2
//
//  Run:
//    ./server          then open http://localhost:8080
// ============================================================================
//
//  API CONTRACT  (frontend app.js depends on these exactly)
//  ---------------------------------------------------------
//  All POST bodies are application/x-www-form-urlencoded.
//  All responses are application/json unless noted.
//  Session is tracked via the request header "X-Session".
//
//  GET  /api/health                 -> { status, time, menu_size, orders }
//  GET  /api/menu                   -> [ { id, name, price, category,
//                                          subcategory, description, tags[],
//                                          emoji, rating, extra{} } ]
//  GET  /api/menu?q=&cat=&sort=     -> filtered list
//  GET  /api/promos                 -> [ { code, description, type, value } ]
//  GET  /api/cart                   -> CART
//  POST /api/cart/add   id,qty,notes-> CART
//  POST /api/cart/update id,qty     -> CART
//  POST /api/cart/remove id         -> CART
//  POST /api/cart/clear             -> CART
//  POST /api/checkout   name,phone,address,email,payment,promo -> ORDER_CONFIRM
//  GET  /api/order/{id}             -> ORDER_STATUS
//  GET  /api/orders                 -> [ ORDER_STATUS, ... ] (recent 20)
//  POST /api/chat       (raw text)  -> text/plain reply
//
//  CART shape:
//    { items:[ {id,name,price,qty,notes,emoji} ],
//      count, subtotal, tax, delivery, discount, discountLabel, total }
//
//  ORDER_CONFIRM shape:
//    { orderId, total, payment, status, eta, message }
//
//  ORDER_STATUS shape:
//    { id, status, eta, placedAt, customer, total, payment,
//      steps:[{name,done,active}], items:[...] }
// ============================================================================

#include "httplib.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace std;

// ============================================================================
//  Utilities — JSON escape, URL decode, form parse, string helpers
// ============================================================================
namespace util {

string jsonEscape(const string& s) {
    string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\b': o += "\\b";  break;
            case '\f': o += "\\f";  break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", (int)c);
                    o += buf;
                } else {
                    o += (char)c;
                }
        }
    }
    return o;
}

inline int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
}

string urlDecode(const string& in) {
    string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        char c = in[i];
        if (c == '+') {
            out += ' ';
        } else if (c == '%' && i + 2 < in.size()) {
            int h = hexVal(in[i + 1]);
            int l = hexVal(in[i + 2]);
            if (h >= 0 && l >= 0) { out += (char)((h << 4) | l); i += 2; }
            else out += c;
        } else {
            out += c;
        }
    }
    return out;
}

map<string, string> parseForm(const string& body) {
    map<string, string> m;
    size_t pos = 0;
    while (pos < body.size()) {
        size_t eq  = body.find('=', pos);
        if (eq == string::npos) break;
        size_t amp = body.find('&', eq);
        string k = body.substr(pos, eq - pos);
        string v = body.substr(eq + 1,
                     amp == string::npos ? string::npos : amp - eq - 1);
        m[urlDecode(k)] = urlDecode(v);
        if (amp == string::npos) break;
        pos = amp + 1;
    }
    return m;
}

string toLower(string s) {
    transform(s.begin(), s.end(), s.begin(),
              [](unsigned char c) { return (char)tolower(c); });
    return s;
}

string trim(const string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) ++a;
    while (b > a && isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

string sanitizeField(string s) {
    for (char& c : s) {
        if (c == '|' || c == '\n' || c == '\r') c = ' ';
    }
    return s;
}

string fmt2(double v) {
    ostringstream ss;
    ss << fixed << setprecision(2) << v;
    return ss.str();
}

} // namespace util


// ============================================================================
//  Domain: Product hierarchy  (Abstraction + Inheritance)
// ============================================================================
class Product {
protected:
    int             id_;
    string          name_;
    double          price_;
    string          category_;
    string          subcategory_;
    string          description_;
    vector<string>  tags_;
    string          emoji_;
    double          rating_;

public:
    Product(int id, string name, double price, string category,
            string subcategory, string description, vector<string> tags,
            string emoji, double rating)
        : id_(id), name_(std::move(name)), price_(price),
          category_(std::move(category)), subcategory_(std::move(subcategory)),
          description_(std::move(description)), tags_(std::move(tags)),
          emoji_(std::move(emoji)), rating_(rating) {}

    virtual ~Product() = default;

    int    getId()          const { return id_; }
    string getName()        const { return name_; }
    double getPrice()       const { return price_; }
    string getCategory()    const { return category_; }
    string getSubcategory() const { return subcategory_; }
    string getDescription() const { return description_; }
    const vector<string>& getTags() const { return tags_; }
    string getEmoji()       const { return emoji_; }
    double getRating()      const { return rating_; }

    virtual string describe()   const = 0;
    virtual double finalPrice() const { return price_; }

    virtual string extraJson() const { return "{}"; }
};

class Coffee : public Product {
    string origin_, roast_;
    bool   decaf_;
public:
    Coffee(int id, string name, double price, string subcategory,
           string description, vector<string> tags, string emoji, double rating,
           string origin, string roast, bool decaf = false)
        : Product(id, std::move(name), price, "coffee", std::move(subcategory),
                  std::move(description), std::move(tags), std::move(emoji), rating),
          origin_(std::move(origin)), roast_(std::move(roast)), decaf_(decaf) {}

    string describe() const override {
        return name_ + " · " + roast_ + " roast · " + origin_;
    }
    string extraJson() const override {
        string j = "{\"origin\":\"" + util::jsonEscape(origin_) +
                   "\",\"roast\":\"" + util::jsonEscape(roast_) +
                   "\",\"decaf\":" + (decaf_ ? "true" : "false") + "}";
        return j;
    }
};

class Tea : public Product {
public:
    Tea(int id, string name, double price, string subcategory,
        string description, vector<string> tags, string emoji, double rating)
        : Product(id, std::move(name), price, "tea", std::move(subcategory),
                  std::move(description), std::move(tags), std::move(emoji), rating) {}
    string describe() const override { return name_ + " · " + subcategory_; }
};

class Pastry : public Product {
public:
    Pastry(int id, string name, double price, string subcategory,
           string description, vector<string> tags, string emoji, double rating)
        : Product(id, std::move(name), price, "pastry", std::move(subcategory),
                  std::move(description), std::move(tags), std::move(emoji), rating) {}
    string describe() const override { return name_; }
};

class Merch : public Product {
public:
    Merch(int id, string name, double price, string subcategory,
          string description, vector<string> tags, string emoji, double rating)
        : Product(id, std::move(name), price, "merch", std::move(subcategory),
                  std::move(description), std::move(tags), std::move(emoji), rating) {}
    string describe() const override { return name_; }
};


// ============================================================================
//  Menu repository — seed data
// ============================================================================
class MenuRepository {
    vector<shared_ptr<Product>> items_;

    void add(shared_ptr<Product> p) { items_.push_back(std::move(p)); }

public:
    MenuRepository() { seed(); }

    const vector<shared_ptr<Product>>& all() const { return items_; }

    shared_ptr<Product> findById(int id) const {
        for (const auto& p : items_) if (p->getId() == id) return p;
        return nullptr;
    }

vector<shared_ptr<Product>> search(const string& q, const string& cat,
                                   const string& sortBy) const {
        string ql = util::toLower(util::trim(q));
        string cl = util::toLower(util::trim(cat));
        vector<shared_ptr<Product>> out;

        for (const auto& p : items_) {
            if (!cl.empty() && p->getCategory() != cl) continue;
            if (!ql.empty()) {
                string hay = util::toLower(p->getName() + " " +
                                           p->getDescription() + " " +
                                           p->getSubcategory() + " " +
                                           p->getCategory());
                for (const auto& t : p->getTags()) hay += " " + util::toLower(t);
                if (hay.find(ql) == string::npos) continue;
            }
            out.push_back(p);
        }

        if (sort == "price_asc") {
            sort(out.begin(), out.end(), [](auto& a, auto& b) {
                return a->getPrice() < b->getPrice(); });
        } else if (sort == "price_desc") {
            sort(out.begin(), out.end(), [](auto& a, auto& b) {
                return a->getPrice() > b->getPrice(); });
        } else if (sort == "rating") {
            sort(out.begin(), out.end(), [](auto& a, auto& b) {
                return a->getRating() > b->getRating(); });
        } else if (sort == "name") {
            sort(out.begin(), out.end(), [](auto& a, auto& b) {
                return a->getName() < b->getName(); });
        }
        return out;
    }

private:
    void seed() {
        // ---------------- ESPRESSO BAR ----------------
        add(make_shared<Coffee>(1, "Espresso", 140, "espresso",
            "Rich, intense single shot pulled from our signature Aurora blend.",
            vector<string>{"bestseller", "strong"}, "☕", 4.7, "Blend", "Dark"));
        add(make_shared<Coffee>(2, "Doppio", 180, "espresso",
            "Double shot — bolder body, deeper crema.",
            vector<string>{"strong"}, "☕", 4.6, "Blend", "Dark"));
        add(make_shared<Coffee>(3, "Americano", 180, "espresso",
            "Espresso diluted with hot water for a smooth, long cup.",
            vector<string>{"classic"}, "☕", 4.4, "Blend", "Medium"));
        add(make_shared<Coffee>(4, "Cappuccino", 220, "espresso",
            "Equal parts espresso, steamed milk and velvet foam.",
            vector<string>{"bestseller"}, "☕", 4.8, "Blend", "Medium"));
        add(make_shared<Coffee>(5, "Caffè Latte", 240, "espresso",
            "Silky steamed milk over a double shot.",
            vector<string>{"bestseller"}, "🥛", 4.7, "Blend", "Medium"));
        add(make_shared<Coffee>(6, "Flat White", 250, "espresso",
            "Ristretto base with thin microfoam. Australian classic.",
            vector<string>{"bestseller"}, "☕", 4.8, "Blend", "Medium"));
        add(make_shared<Coffee>(7, "Macchiato", 200, "espresso",
            "Espresso 'stained' with a dollop of milk foam.",
            vector<string>{"classic"}, "☕", 4.3, "Blend", "Dark"));
        add(make_shared<Coffee>(8, "Cortado", 210, "espresso",
            "Espresso cut with equal warm milk. Balanced and sweet.",
            vector<string>{"classic"}, "☕", 4.5, "Blend", "Medium"));
        add(make_shared<Coffee>(9, "Mocha", 280, "espresso",
            "Espresso, 55% dark chocolate, steamed milk, cocoa dust.",
            vector<string>{"sweet"}, "🍫", 4.6, "Blend", "Medium"));
        add(make_shared<Coffee>(10, "Caramel Macchiato", 290, "espresso",
            "Vanilla, milk, espresso, and a caramel drizzle.",
            vector<string>{"sweet", "bestseller"}, "🍮", 4.7, "Blend", "Medium"));

        // ---------------- BREW BAR ----------------
        add(make_shared<Coffee>(11, "Ethiopian Yirgacheffe", 320, "brewed",
            "Light roast, floral jasmine, bright citrus. Pour over.",
            vector<string>{"single-origin", "fruity"}, "🌸", 4.9,
            "Ethiopia", "Light"));
        add(make_shared<Coffee>(12, "Colombian Supremo", 280, "brewed",
            "Medium roast, caramel and toasted nuts. Drip.",
            vector<string>{"single-origin"}, "🇨🇴", 4.6, "Colombia", "Medium"));
        add(make_shared<Coffee>(13, "Kenya AA", 330, "brewed",
            "Bright acidity, blackcurrant and grapefruit. Pour over.",
            vector<string>{"single-origin", "fruity"}, "🇰🇪", 4.8, "Kenya", "Medium"));
        add(make_shared<Coffee>(14, "Sumatra Mandheling", 300, "brewed",
            "Dark roast, earthy, full body, low acidity. French press.",
            vector<string>{"single-origin", "strong"}, "🇮🇩", 4.5, "Indonesia", "Dark"));
        add(make_shared<Coffee>(15, "Brazil Santos", 260, "brewed",
            "Smooth, chocolatey, mild. Everyday drip.",
            vector<string>{"single-origin"}, "🇧🇷", 4.4, "Brazil", "Medium"));
        add(make_shared<Coffee>(16, "Guatemala Antigua", 310, "brewed",
            "Smoky, spicy, complex with a cocoa finish.",
            vector<string>{"single-origin"}, "🇬🇹", 4.6, "Guatemala", "Medium"));
        add(make_shared<Coffee>(17, "Ethiopian Decaf", 300, "brewed",
            "Swiss-water decaf. All the flavour, none of the buzz.",
            vector<string>{"decaf", "single-origin"}, "🌙", 4.4,
            "Ethiopia", "Light", true));

        // ---------------- COLD BAR ----------------
        add(make_shared<Coffee>(18, "Cold Brew", 260, "cold",
            "18-hour steeped. Smooth, low-acid, naturally sweet.",
            vector<string>{"vegan", "bestseller"}, "🧊", 4.8, "Blend", "Dark"));
        add(make_shared<Coffee>(19, "Nitro Cold Brew", 320, "cold",
            "Nitrogen-infused for a creamy, stout-like pour. No ice.",
            vector<string>{"vegan", "new"}, "🧊", 4.9, "Blend", "Dark"));
        add(make_shared<Coffee>(20, "Iced Latte", 250, "cold",
            "Double shot over ice with cold milk.",
            vector<string>{}, "🧊", 4.5, "Blend", "Medium"));
        add(make_shared<Coffee>(21, "Iced Americano", 200, "cold",
            "Chilled espresso and water. Clean and refreshing.",
            vector<string>{"vegan"}, "🧊", 4.3, "Blend", "Medium"));
        add(make_shared<Coffee>(22, "Vietnamese Cold Brew", 300, "cold",
            "Dark roast, condensed milk, plenty of ice.",
            vector<string>{"sweet"}, "🧊", 4.7, "Vietnam", "Dark"));
        add(make_shared<Coffee>(23, "Affogato", 280, "cold",
            "Vanilla gelato drowned in a fresh double shot.",
            vector<string>{"sweet", "dessert"}, "🍨", 4.8, "Blend", "Dark"));

        // ---------------- TEA & OTHER ----------------
        add(make_shared<Tea>(24, "Matcha Latte", 280, "hot",
            "Ceremonial-grade matcha whisked into steamed milk.",
            vector<string>{"vegan-option"}, "🍵", 4.6));
        add(make_shared<Tea>(25, "Chai Latte", 220, "hot",
            "House-brewed masala chai with steamed milk.",
            vector<string>{"bestseller"}, "🍵", 4.5));
        add(make_shared<Tea>(26, "Dirty Chai", 260, "hot",
            "Our chai with a shot of espresso. Best of both.",
            vector<string>{}, "🍵", 4.6));
        add(make_shared<Tea>(27, "Hot Chocolate", 240, "hot",
            "70% dark chocolate melted into steamed milk.",
            vector<string>{"sweet"}, "🍫", 4.7));

        // ---------------- PASTRIES ----------------
        add(make_shared<Pastry>(28, "Butter Croissant", 180, "viennoiserie",
            "Flaky, buttery, laminated 27 layers. Baked fresh daily.",
            vector<string>{"bestseller"}, "🥐", 4.7));
        add(make_shared<Pastry>(29, "Almond Croissant", 220, "viennoiserie",
            "Filled with frangipane, topped with toasted almonds.",
            vector<string>{"bestseller"}, "🥐", 4.8));
        add(make_shared<Pastry>(30, "Pain au Chocolat", 200, "viennoiserie",
            "Buttery pastry wrapped around two batons of dark chocolate.",
            vector<string>{}, "🥐", 4.7));
        add(make_shared<Pastry>(31, "Cheese Danish", 210, "viennoiserie",
            "Cream cheese filling, vanilla glaze, hint of lemon.",
            vector<string>{}, "🧀", 4.5));
        add(make_shared<Pastry>(32, "Cinnamon Roll", 200, "sweet",
            "Warm, gooey, cream-cheese frosted. Cinnamon-forward.",
            vector<string>{"sweet"}, "🍥", 4.6));
        add(make_shared<Pastry>(33, "Blueberry Muffin", 160, "muffin",
            "Wild blueberries with a brown-sugar streusel top.",
            vector<string>{}, "🧁", 4.4));
        add(make_shared<Pastry>(34, "Chocolate Chip Muffin", 160, "muffin",
            "Double chocolate batter, semi-sweet chips.",
            vector<string>{"sweet"}, "🧁", 4.4));
        add(make_shared<Pastry>(35, "Banana Bread", 170, "cake",
            "Moist, walnut-studded, cinnamon-kissed. Served warm.",
            vector<string>{}, "🍞", 4.5));

        // ---------------- SAVORY ----------------
        add(make_shared<Pastry>(36, "Ham & Cheese Croissant", 260, "savory",
            "Smoked ham, melted Gruyère, Dijon butter.",
            vector<string>{"bestseller"}, "🥐", 4.7));
        add(make_shared<Pastry>(37, "Spinach & Feta Puff", 220, "savory",
            "Flaky pastry filled with spinach, feta, and dill.",
            vector<string>{"vegetarian"}, "🥬", 4.5));
        add(make_shared<Pastry>(38, "Avocado Toast", 280, "savory",
            "Sourdough, smashed avocado, chili flakes, lemon zest.",
            vector<string>{"vegan"}, "🥑", 4.6));
        add(make_shared<Pastry>(39, "Egg & Cheese Sandwich", 240, "savory",
            "Brioche bun, scrambled eggs, sharp cheddar, black pepper.",
            vector<string>{}, "🥪", 4.5));
        add(make_shared<Pastry>(40, "Bagel & Cream Cheese", 180, "savory",
            "New-York-style bagel, whipped cream cheese, chives.",
            vector<string>{"vegetarian"}, "🥯", 4.4));

        // ---------------- DESSERTS ----------------
        add(make_shared<Pastry>(41, "Chocolate Chip Cookie", 120, "dessert",
            "Brown-butter dough, sea-salt finish, gooey centre.",
            vector<string>{"bestseller"}, "🍪", 4.8));
        add(make_shared<Pastry>(42, "Almond Biscotti", 140, "dessert",
            "Twice-baked, crunchy. Perfect for dipping in espresso.",
            vector<string>{}, "🍪", 4.3));
        add(make_shared<Pastry>(43, "Brownie", 160, "dessert",
            "Fudgy, 70% dark chocolate, cracked top.",
            vector<string>{"sweet"}, "🍫", 4.7));
        add(make_shared<Pastry>(44, "Macaron Set (3)", 250, "dessert",
            "Pistachio, raspberry, and vanilla bean.",
            vector<string>{"gift"}, "🍬", 4.6));
        add(make_shared<Pastry>(45, "Tiramisu", 280, "dessert",
            "Espresso-soaked ladyfingers, mascarpone, cocoa.",
            vector<string>{"bestseller"}, "🍰", 4.9));
        add(make_shared<Pastry>(46, "Banana Walnut Loaf", 170, "dessert",
            "Toasted walnuts, cinnamon glaze, moist crumb.",
            vector<string>{}, "🍞", 4.4));

        // ---------------- MERCH ----------------
        add(make_shared<Merch>(47, "Aurora Ceramic Mug", 450, "drinkware",
            "Handthrown 350 ml stoneware. Dishwasher safe.",
            vector<string>{"gift"}, "☕", 4.8));
        add(make_shared<Merch>(48, "Vacuum Travel Tumbler", 890, "drinkware",
            "Insulated stainless steel. 12 h hot / 24 h cold.",
            vector<string>{"gift"}, "🥤", 4.7));
        add(make_shared<Merch>(49, "Aurora Blend Beans 250g", 550, "beans",
            "Our signature house blend. Whole bean, roasted weekly.",
            vector<string>{"gift", "bestseller"}, "🫘", 4.9));
    }
};


// ============================================================================
//  Cart  (Encapsulation)
// ============================================================================
struct CartItem {
    int    productId;
    string name;
    double price;
    int    qty;
    string notes;
    string emoji;
};

class Cart {
    vector<CartItem> items_;
public:
    void add(int id, const string& name, double price, int qty,
             const string& notes, const string& emoji) {
        if (qty <= 0) qty = 1;
        for (auto& it : items_) {
            if (it.productId == id && it.notes == notes) {
                it.qty += qty;
                if (it.qty > 99) it.qty = 99;
                return;
            }
        }
        if (items_.size() >= 50) return;
        items_.push_back({id, name, price, qty, notes, emoji});
    }

    bool update(int id, int qty) {
        for (auto& it : items_) {
            if (it.productId == id) {
                if (qty <= 0) return remove(id);
                it.qty = min(qty, 99);
                return true;
            }
        }
        return false;
    }

    bool remove(int id) {
        for (auto it = items_.begin(); it != items_.end(); ++it) {
            if (it->productId == id) { items_.erase(it); return true; }
        }
        return false;
    }

    void clear() { items_.clear(); }

    double subtotal() const {
        double s = 0;
        for (const auto& it : items_) s += it.price * it.qty;
        return s;
    }

    int count() const {
        int c = 0;
        for (const auto& it : items_) c += it.qty;
        return c;
    }

    bool empty() const { return items_.empty(); }
    const vector<CartItem>& items() const { return items_; }
};


// ============================================================================
//  Discount  (Strategy pattern — Polymorphism)
// ============================================================================
class Discount {
public:
    virtual ~Discount() = default;
    virtual double apply(double subtotal) const = 0;
    virtual string label() const = 0;
};

class PercentOff : public Discount {
    double pct_;
public:
    explicit PercentOff(double p) : pct_(p) {}
    double apply(double s) const override { return s * (1.0 - pct_ / 100.0); }
    string label() const override { return to_string((int)pct_) + "% off"; }
};

class FlatOff : public Discount {
    double amt_;
public:
    explicit FlatOff(double a) : amt_(a) {}
    double apply(double s) const override { return max(0.0, s - amt_); }
    string label() const override { return "Flat ₹" + to_string((int)amt_) + " off"; }
};


// ============================================================================
//  Payment  (Polymorphism)
// ============================================================================
class PaymentMethod {
public:
    virtual ~PaymentMethod() = default;
    virtual bool   process(double amount) = 0;
    virtual string name() const = 0;
};

class OnlinePayment : public PaymentMethod {
public:
    bool process(double amount) override { return amount > 0 && amount < 100000; }
    string name() const override { return "Online"; }
};

class CashOnDelivery : public PaymentMethod {
public:
    bool process(double) override { return true; }
    string name() const override { return "COD"; }
};


// ============================================================================
//  Order + state machine
// ============================================================================
enum class Status { PLACED, CONFIRMED, BREWING, OUT_FOR_DELIVERY, DELIVERED, CANCELLED };

inline string statusString(Status s) {
    switch (s) {
        case Status::PLACED:           return "Placed";
        case Status::CONFIRMED:        return "Confirmed";
        case Status::BREWING:          return "Brewing";
        case Status::OUT_FOR_DELIVERY: return "Out for delivery";
        case Status::DELIVERED:        return "Delivered";
        case Status::CANCELLED:        return "Cancelled";
    }
    return "Unknown";
}

// Seconds elapsed since placed -> current status
inline Status deriveStatus(time_t placedAt, Status current) {
    if (current == Status::CANCELLED) return current;
    long e = (long)(time(nullptr) - placedAt);
    if (e <  5) return Status::PLACED;
    if (e < 15) return Status::CONFIRMED;
    if (e < 30) return Status::BREWING;
    if (e < 50) return Status::OUT_FOR_DELIVERY;
    return Status::DELIVERED;
}

inline int deriveEta(time_t placedAt) {
    long e = (long)(time(nullptr) - placedAt);
    int eta = 50 - (int)e;
    return eta < 0 ? 0 : eta;
}

inline bool canCancel(time_t placedAt) {
    return (time(nullptr) - placedAt) < 15;
}

struct Order {
    string           id;
    string           customer;
    string           phone;
    string           address;
    string           email;
    string           payment;
    double           subtotal   = 0;
    double           discount   = 0;
    string           discountLabel;
    double           tax        = 0;
    double           delivery   = 0;
    double           total      = 0;
    vector<CartItem> items;
    Status           status     = Status::PLACED;
    time_t           placedAt   = 0;
};


// ============================================================================
//  Session store  (Singleton, thread-safe)
// ============================================================================
class SessionStore {
    map<string, Cart> carts_;
    mutex             mu_;
    SessionStore() = default;

public:
    static SessionStore& instance() {
        static SessionStore s;
        return s;
    }
    SessionStore(const SessionStore&) = delete;
    SessionStore& operator=(const SessionStore&) = delete;

    Cart& get(const string& sid) {
        lock_guard<mutex> lk(mu_);
        return carts_[sid];
    }
};


// ============================================================================
//  Order repository  — in-memory + simple persistence
// ============================================================================
class OrderRepository {
    vector<Order> orders_;
    mutex         mu_;
    string        path_;
    atomic<int>   seq_{10000};

    OrderRepository() : path_("data/orders.dat") {
        load();
    }

    static string esc(const string& s) {
        string o;
        for (char c : s) {
            if (c == '|')       o += "\\p";
            else if (c == '\\') o += "\\\\";
            else if (c == '\n') o += "\\n";
            else if (c == '\r') o += "\\r";
            else                o += c;
        }
        return o;
    }
    static string unesc(const string& s) {
        string o;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '\\' && i + 1 < s.size()) {
                char n = s[++i];
                if (n == 'p') o += '|';
                else if (n == 'n') o += '\n';
                else if (n == 'r') o += '\r';
                else o += n;
            } else o += s[i];
        }
        return o;
    }
    static vector<string> split(const string& s, char d) {
        vector<string> out;
        string cur;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '\\' && i + 1 < s.size()) { cur += s[i]; cur += s[++i]; }
            else if (s[i] == d) { out.push_back(cur); cur.clear(); }
            else cur += s[i];
        }
        out.push_back(cur);
        return out;
    }

    void load() {
        ifstream f(path_);
        if (!f) return;
        string line;
        map<string, Order> byId;
        vector<string> orderOf;
        while (getline(f, line)) {
            if (line.empty()) continue;
            auto p = split(line, '|');
            if (p.empty()) continue;
            if (p[0] == "O" && p.size() >= 11) {
                Order o;
                o.id            = p[1];
                o.customer      = unesc(p[2]);
                o.phone         = unesc(p[3]);
                o.address       = unesc(p[4]);
                o.email         = unesc(p[5]);
                o.payment       = p[6];
                o.subtotal      = stod(p[7]);
                o.discount      = stod(p[8]);
                o.discountLabel = unesc(p[9]);
                o.tax           = stod(p[10]);
                o.delivery      = stod(p[11]);
                o.total         = stod(p[12]);
                o.status        = (Status)stoi(p[13]);
                o.placedAt      = (time_t)stol(p[14]);
                byId[o.id] = o;
                orderOf.push_back(o.id);
            } else if (p[0] == "I" && p.size() >= 8) {
                string oid = p[1];
                if (byId.count(oid)) {
                    CartItem ci;
                    ci.productId = stoi(p[2]);
                    ci.name      = unesc(p[3]);
                    ci.price     = stod(p[4]);
                    ci.qty       = stoi(p[5]);
                    ci.notes     = unesc(p[6]);
                    ci.emoji     = unesc(p[7]);
                    byId[oid].items.push_back(ci);
                }
            } else if (p[0] == "S" && p.size() >= 2) {
                seq_.store(stoi(p[1]));
            }
        }
        for (const auto& id : orderOf) orders_.push_back(byId[id]);
    }

public:
    static OrderRepository& instance() {
        static OrderRepository r;
        return r;
    }
    OrderRepository(const OrderRepository&) = delete;
    OrderRepository& operator=(const OrderRepository&) = delete;

    string nextId() {
        int n = ++seq_;
        ostringstream ss;
        ss << "CFE-" << n;
        return ss.str();
    }

    void add(const Order& o) {
        lock_guard<mutex> lk(mu_);
        orders_.push_back(o);
        saveLocked();
    }

    bool get(const string& id, Order& out) {
        lock_guard<mutex> lk(mu_);
        for (auto& o : orders_) if (o.id == id) { out = o; return true; }
        return false;
    }

    vector<Order> recent(int n) {
        lock_guard<mutex> lk(mu_);
        vector<Order> out;
        int start = max(0, (int)orders_.size() - n);
        for (int i = (int)orders_.size() - 1; i >= start; --i)
            out.push_back(orders_[i]);
        return out;
    }

    bool cancel(const string& id) {
        lock_guard<mutex> lk(mu_);
        for (auto& o : orders_) {
            if (o.id == id) {
                if (!canCancel(o.placedAt)) return false;
                o.status = Status::CANCELLED;
                saveLocked();
                return true;
            }
        }
        return false;
    }

    size_t size() const { return orders_.size(); }

private:
    void saveLocked() {
        // Ensure data/ exists
        #ifdef _WIN32
            system("if not exist data mkdir data >nul 2>nul");
        #else
            system("mkdir -p data");
        #endif
        ofstream f(path_, ios::trunc);
        if (!f) return;
        f << "S|" << seq_.load() << "\n";
        for (const auto& o : orders_) {
            f << "O|" << o.id << "|" << esc(o.customer) << "|" << esc(o.phone)
              << "|" << esc(o.address) << "|" << esc(o.email) << "|"
              << o.payment << "|" << o.subtotal << "|" << o.discount << "|"
              << esc(o.discountLabel) << "|" << o.tax << "|" << o.delivery
              << "|" << o.total << "|" << (int)o.status << "|"
              << (long)o.placedAt << "\n";
            for (const auto& it : o.items) {
                f << "I|" << o.id << "|" << it.productId << "|"
                  << esc(it.name) << "|" << it.price << "|" << it.qty << "|"
                  << esc(it.notes) << "|" << esc(it.emoji) << "\n";
            }
        }
    }
};


// ============================================================================
//  CoffeeBot — rule-based conversational agent (your "AI avatar")
// ============================================================================
class CoffeeBot {
public:
    string respond(const string& raw) const {
        string m = util::toLower(util::trim(raw));
        if (m.empty()) return "Hi! Ask me about our menu, delivery, promos, or tracking.";

        auto has = [&](const initializer_list<const char*>& keys) {
            for (auto k : keys) if (m.find(k) != string::npos) return true;
            return false;
        };

        if (has({"hello","hi ","hey","namaste","good morning","good evening"}))
            return "Hey there! ☕ I'm Aria, your coffee concierge. Want a recommendation, or should I walk you through delivery and promos?";

        if (has({"thank","thanks","thx"}))
            return "Anytime! Enjoy your cup. ☕";

        if (has({"recommend","suggest","best","what should","popular","bestseller"}))
            return "Try our Ethiopian Yirgacheffe pour over — floral, bright, and complex. For something cold, the Nitro Cold Brew is silky and low-acid. Pair either with an Almond Croissant.";

        if (has({"delivery","how long","eta","when will","arrive","ship"}))
            return "Standard delivery is ~50 seconds in this demo — you'll see a live ETA on the tracking page. Real-world it'd be 25–35 min.";

        if (has({"track","where is","order status","my order"}))
            return "Head to the Track page and paste your order ID (it looks like CFE-10001). The stepper updates every 2 seconds.";

        if (has({"promo","discount","coupon","code","offer"}))
            return "Two live codes: BREW20 for 20% off your cart, or FIRSTCUP for a flat ₹50 off. Apply at checkout.";

        if (has({"vegan","dairy","lactose","plant"}))
            return "Our Cold Brew, Nitro, Iced Americano, and Avocado Toast are vegan by default. Any milk drink can use oat milk — just add it in the notes.";

        if (has({"decaf","caffeine","no caffeine"}))
            return "Yes — we brew an Ethiopian Decaf (Swiss-water process) with all the flavour and none of the buzz.";

        if (has({"hours","open","close","timing"}))
            return "We're open 24/7 in this demo. In real life you'd catch us 7 am – 11 pm, every day.";

        if (has({"cancel","refund","money back"}))
            return "You can cancel for free within 15 seconds of placing the order. After brewing starts, it's on its way!";

        if (has({"payment","pay","cod","cash","online","card","upi"}))
            return "We accept Online payment (simulated) and Cash on Delivery. Both work identically in this demo.";

        if (has({"menu","what do you","food","eat","drink"}))
            return "We've got espresso drinks, single-origin pour-overs, cold brew, matcha, chai, pastries, savory bites, desserts, and merch. Try the search bar — type 'ethiopian' or 'cold'.";

        if (has({"strong","bold","kick"}))
            return "Go for the Doppio or Sumatra Mandheling French press. Both pack a serious punch.";

        if (has({"sweet","dessert","chocolate"}))
            return "The Tiramisu is our most-loved dessert — espresso-soaked and dusted with cocoa. The Brownie is fudgy and rich too.";

        return "I can help with: menu recommendations, delivery ETA, order tracking, promo codes, vegan options, decaf, payments, and cancellations. What would you like?";
    }
};


// ============================================================================
//  JSON serializers
// ============================================================================
namespace json {

string product(const shared_ptr<Product>& p) {
    string j = "{";
    j += "\"id\":"          + to_string(p->getId())              + ",";
    j += "\"name\":\""      + util::jsonEscape(p->getName())     + "\",";
    j += "\"price\":"       + to_string((int)p->getPrice())      + ",";
    j += "\"category\":\""  + util::jsonEscape(p->getCategory()) + "\",";
    j += "\"subcategory\":\"" + util::jsonEscape(p->getSubcategory()) + "\",";
    j += "\"description\":\"" + util::jsonEscape(p->getDescription()) + "\",";
    j += "\"emoji\":\""     + util::jsonEscape(p->getEmoji())    + "\",";
    j += "\"rating\":"      + util::fmt2(p->getRating())         + ",";
    j += "\"tags\":[";
    const auto& tags = p->getTags();
    for (size_t i = 0; i < tags.size(); ++i) {
        j += "\"" + util::jsonEscape(tags[i]) + "\"";
        if (i + 1 < tags.size()) j += ",";
    }
    j += "],";
    j += "\"extra\":" + p->extraJson();
    j += "}";
    return j;
}

string cart(const Cart& c, double discountAmt = 0,
            const string& discountLabel = "") {
    double sub  = c.subtotal();
    double disc = discountAmt;
    double taxable = max(0.0, sub - disc);
    double tax  = taxable * 0.05;
    double del  = c.empty() ? 0 : 40;
    double tot  = taxable + tax + del;

    string j = "{\"items\":[";
    const auto& items = c.items();
    for (size_t i = 0; i < items.size(); ++i) {
        const auto& it = items[i];
        j += "{";
        j += "\"id\":"    + to_string(it.productId)            + ",";
        j += "\"name\":\"" + util::jsonEscape(it.name)         + "\",";
        j += "\"price\":" + to_string((int)it.price)           + ",";
        j += "\"qty\":"   + to_string(it.qty)                  + ",";
        j += "\"notes\":\"" + util::jsonEscape(it.notes)       + "\",";
        j += "\"emoji\":\"" + util::jsonEscape(it.emoji)       + "\"";
        j += "}";
        if (i + 1 < items.size()) j += ",";
    }
    j += "],";
    j += "\"count\":"         + to_string(c.count())          + ",";
    j += "\"subtotal\":"      + to_string((int)sub)           + ",";
    j += "\"discount\":"      + to_string((int)disc)          + ",";
    j += "\"discountLabel\":\"" + util::jsonEscape(discountLabel) + "\",";
    j += "\"tax\":"           + to_string((int)tax)           + ",";
    j += "\"delivery\":"      + to_string((int)del)           + ",";
    j += "\"total\":"         + to_string((int)tot)           + "}";
    return j;
}

string orderStatus(const Order& o) {
    Status s = deriveStatus(o.placedAt, o.status);
    int eta  = deriveEta(o.placedAt);

    // step names must match frontend stepper
    const char* steps[] = {"Placed","Confirmed","Brewing","Out for delivery","Delivered"};
    int activeIdx = -1;
    switch (s) {
        case Status::PLACED:           activeIdx = 0; break;
        case Status::CONFIRMED:        activeIdx = 1; break;
        case Status::BREWING:          activeIdx = 2; break;
        case Status::OUT_FOR_DELIVERY: activeIdx = 3; break;
        case Status::DELIVERED:        activeIdx = 4; break;
        case Status::CANCELLED:        activeIdx = -2; break;
    }

    string j = "{";
    j += "\"id\":\""       + util::jsonEscape(o.id)       + "\",";
    j += "\"status\":\""   + util::jsonEscape(statusString(s)) + "\",";
    j += "\"eta\":"        + to_string(eta)               + ",";
    j += "\"placedAt\":"   + to_string((long)o.placedAt)  + ",";
    j += "\"customer\":\"" + util::jsonEscape(o.customer) + "\",";
    j += "\"address\":\""  + util::jsonEscape(o.address)  + "\",";
    j += "\"total\":"      + to_string((int)o.total)      + ",";
    j += "\"payment\":\""  + util::jsonEscape(o.payment)  + "\",";
    j += "\"canCancel\":"  + string(canCancel(o.placedAt) ? "true" : "false") + ",";

    j += "\"steps\":[";
    for (int i = 0; i < 5; ++i) {
        bool done   = (activeIdx >= 0 && i < activeIdx);
        bool active = (i == activeIdx);
        j += "{\"name\":\"" + string(steps[i]) + "\",\"done\":";
        j += (done ? "true" : "false");
        j += ",\"active\":";
        j += (active ? "true" : "false");
        j += "}";
        if (i < 4) j += ",";
    }
    j += "],";

    j += "\"items\":[";
    for (size_t i = 0; i < o.items.size(); ++i) {
        const auto& it = o.items[i];
        j += "{";
        j += "\"id\":"    + to_string(it.productId)          + ",";
        j += "\"name\":\"" + util::jsonEscape(it.name)       + "\",";
        j += "\"price\":" + to_string((int)it.price)         + ",";
        j += "\"qty\":"   + to_string(it.qty)                + ",";
        j += "\"emoji\":\"" + util::jsonEscape(it.emoji)     + "\"";
        j += "}";
        if (i + 1 < o.items.size()) j += ",";
    }
    j += "]}";
    return j;
}

} // namespace json


// ============================================================================
//  Promo codes
// ============================================================================
struct Promo {
    string code;
    string description;
    string type;     // "percent" | "flat"
    double value;
};

static const vector<Promo> PROMOS = {
    {"BREW20",   "20% off your entire cart (max ₹500)", "percent", 20},
    {"FIRSTCUP", "Flat ₹50 off your first order",       "flat",    50},
    {"AURORA10", "10% off — loyalty members",           "percent", 10},
};

static unique_ptr<Discount> makeDiscount(const string& code,
                                         double subtotal,
                                         string& labelOut) {
    string c = util::toLower(util::trim(code));
    if (c.empty()) { labelOut = ""; return nullptr; }

    for (const auto& p : PROMOS) {
        string pc = util::toLower(p.code);
        if (pc != c) continue;
        if (p.type == "percent") {
            double capped = min(subtotal * p.value / 100.0, 500.0);
            labelOut = to_string((int)p.value) + "% off";
            // Return a percent discounter — but cap after. We'll cap manually
            // by constructing PercentOff and separately clamping.
            return make_unique<PercentOff>(p.value);
        }
        labelOut = "Flat ₹" + to_string((int)p.value) + " off";
        return make_unique<FlatOff>(p.value);
    }
    labelOut = "";
    return nullptr;
}


// ============================================================================
//  Helpers for HTTP handlers
// ============================================================================
static string sessionOf(const httplib::Request& req) {
    string sid = req.get_header_value("X-Session");
    if (sid.empty() || sid.size() > 64) sid = "guest";
    return sid;
}

static void sendJson(httplib::Response& res, const string& body, int status = 200) {
    res.status = status;
    res.set_content(body, "application/json");
}

static void errJson(httplib::Response& res, int code, const string& msg) {
    res.status = code;
    res.set_content("{\"error\":\"" + util::jsonEscape(msg) + "\"}", "application/json");
}

// Extract "id" from a raw body regardless of form vs JSON-ish
static int extractInt(const map<string,string>& f, const string& k, int def = 0) {
    auto it = f.find(k);
    if (it == f.end()) return def;
    try { return stoi(it->second); } catch (...) { return def; }
}
static double extractD(const map<string,string>& f, const string& k, double def = 0) {
    auto it = f.find(k);
    if (it == f.end()) return def;
    try { return stod(it->second); } catch (...) { return def; }
}
static string extractS(const map<string,string>& f, const string& k,
                       const string& def = "") {
    auto it = f.find(k);
    return it == f.end() ? def : it->second;
}


// ============================================================================
//  MAIN — server bootstrap & routes
// ============================================================================
int main(int argc, char** argv) {
    int port = 8080;
if (const char* envPort = std::getenv("PORT")) {
    port = std::atoi(envPort);
}
    if (argc > 1) { try { port = stoi(argv[1]); } catch (...) {} }

    MenuRepository&  menu = *new MenuRepository();     // intentional leak: lifetime == process
    OrderRepository& repo = OrderRepository::instance();
    CoffeeBot        bot;

    httplib::Server svr;

    // -------- Logging --------
    svr.set_logger([](const httplib::Request& req, const httplib::Response& res) {
        printf("[%s] %s -> %d\n", req.method.c_str(), req.path.c_str(), res.status);
        fflush(stdout);
    });

    // -------- Static files (index.html, style.css, app.js live next to server) --------
    svr.set_mount_point("/", "./");

    // -------- Health --------
    svr.Get("/api/health", [&](const httplib::Request&, httplib::Response& res) {
        string j = "{\"status\":\"ok\",\"time\":" + to_string((long)time(nullptr)) +
                   ",\"menu_size\":" + to_string(menu.all().size()) +
                   ",\"orders\":" + to_string(repo.size()) + "}";
       sendJson(res, j);
    });

    // -------- Menu --------
    svr.Get("/api/menu", [&](const httplib::Request& req, httplib::Response& res) {
        string q    = req.has_param("q")    ? req.get_param_value("q")    : "";
        string cat  = req.has_param("cat")  ? req.get_param_value("cat")  : "";
        string sort = req.has_param("sort") ? req.get_param_value("sort") : "";

        auto items = menu.search(q, cat, sort);
        string j = "[";
        for (size_t i = 0; i < items.size(); ++i) {
            j += json::product(items[i]);
            if (i + 1 < items.size()) j += ",";
        }
        j += "]";
       sendJson(res, j);
    });

    // -------- Promos --------
    svr.Get("/api/promos", [](const httplib::Request&, httplib::Response& res) {
        string j = "[";
        for (size_t i = 0; i < PROMOS.size(); ++i) {
            const auto& p = PROMOS[i];
            j += "{\"code\":\"" + util::jsonEscape(p.code) +
                 "\",\"description\":\"" + util::jsonEscape(p.description) +
                 "\",\"type\":\"" + p.type +
                 "\",\"value\":" + util::fmt2(p.value) + "}";
            if (i + 1 < PROMOS.size()) j += ",";
        }
        j += "]";
        sendJson(res, j);
    });

    // -------- Cart (GET) --------
    svr.Get("/api/cart", [&](const httplib::Request& req, httplib::Response& res) {
        string sid = sessionOf(req);
        Cart& c = SessionStore::instance().get(sid);
        lock_guard<mutex> lk(*new mutex()); // placeholder; Cart is only touched per-session
        sendJson(res, json::cart(c));
    });

    // -------- Cart: add --------
    svr.Post("/api/cart/add", [&](const httplib::Request& req, httplib::Response& res) {
        string sid = sessionOf(req);
        auto f = util::parseForm(req.body);
        int id  = extractInt(f, "id", -1);
        int qty = extractInt(f, "qty", 1);
        string notes = util::trim(extractS(f, "notes"));
        if (notes.size() > 200) notes = notes.substr(0, 200);

        auto p = menu.findById(id);
        if (!p) { errJson(res, 404, "Unknown product"); return; }
        if (qty < 1) qty = 1;
        if (qty > 20) qty = 20;

        Cart& c = SessionStore::instance().get(sid);
        c.add(p->getId(), p->getName(), p->getPrice(), qty, notes, p->getEmoji());
        sendJson(res, json::cart(c));
    });

    // -------- Cart: update qty --------
    svr.Post("/api/cart/update", [&](const httplib::Request& req, httplib::Response& res) {
        string sid = sessionOf(req);
        auto f = util::parseForm(req.body);
        int id  = extractInt(f, "id", -1);
        int qty = extractInt(f, "qty", 1);
        Cart& c = SessionStore::instance().get(sid);
        c.update(id, qty);
       sendJson(res, json::cart(c));
    });

    // -------- Cart: remove --------
    svr.Post("/api/cart/remove", [&](const httplib::Request& req, httplib::Response& res) {
        string sid = sessionOf(req);
        auto f = util::parseForm(req.body);
        int id = extractInt(f, "id", -1);
        Cart& c = SessionStore::instance().get(sid);
        c.remove(id);
        sendJson(res, json::cart(c));
    });

    // -------- Cart: clear --------
    svr.Post("/api/cart/clear", [&](const httplib::Request& req, httplib::Response& res) {
        string sid = sessionOf(req);
        Cart& c = SessionStore::instance().get(sid);
        c.clear();
        sendJson(res, json::cart(c));
    });

    // -------- Checkout --------
    svr.Post("/api/checkout", [&](const httplib::Request& req, httplib::Response& res) {
        string sid = sessionOf(req);
        auto f = util::parseForm(req.body);

        string name    = util::trim(extractS(f, "name"));
        string phone   = util::trim(extractS(f, "phone"));
        string address = util::trim(extractS(f, "address"));
        string email   = util::trim(extractS(f, "email"));
        string payment = util::toLower(util::trim(extractS(f, "payment", "online")));
        string promo   = util::trim(extractS(f, "promo"));

        if (name.empty() || name.size() < 2)
            return errJson(res, 400, "Please enter your full name.");
        if (phone.size() < 8)
            return errJson(res, 400, "Please enter a valid phone number.");
        if (address.size() < 8)
            return errJson(res, 400, "Please enter a complete delivery address.");
        if (payment != "online" && payment != "cod")
            return errJson(res, 400, "Invalid payment method.");

        Cart& c = SessionStore::instance().get(sid);
        if (c.empty()) return errJson(res, 400, "Your cart is empty.");

        double sub = c.subtotal();

        string discLabel;
        unique_ptr<Discount> disc = makeDiscount(promo, sub, discLabel);
        double discAmt = 0;
        if (disc) {
            discAmt = sub - disc->apply(sub);
            // Cap percent discount at ₹500
            if (discAmt > 500) discAmt = 500;
        }

        double taxable = max(0.0, sub - discAmt);
        double tax     = taxable * 0.05;
        double del     = 40;
        double total   = taxable + tax + del;

        unique_ptr<PaymentMethod> pm =
            (payment == "cod")
                ? (unique_ptr<PaymentMethod>)make_unique<CashOnDelivery>()
                : (unique_ptr<PaymentMethod>)make_unique<OnlinePayment>();

        if (!pm->process(total))
            return errJson(res, 400, "Payment could not be processed.");

        Order o;
        o.id            = repo.nextId();
        o.customer      = util::sanitizeField(name);
        o.phone         = util::sanitizeField(phone);
        o.address       = util::sanitizeField(address);
        o.email         = util::sanitizeField(email);
        o.payment       = pm->name();
        o.subtotal      = sub;
        o.discount      = discAmt;
        o.discountLabel = discLabel;
        o.tax           = tax;
        o.delivery      = del;
        o.total         = total;
        o.items         = c.items();
        o.status        = Status::PLACED;
        o.placedAt      = time(nullptr);

        repo.add(o);
        c.clear();

        string j = "{";
        j += "\"orderId\":\"" + util::jsonEscape(o.id) + "\",";
        j += "\"total\":"     + to_string((int)total)   + ",";
        j += "\"payment\":\"" + util::jsonEscape(o.payment) + "\",";
        j += "\"status\":\"Placed\",";
        j += "\"eta\":50,";
        j += "\"message\":\"Order confirmed. We'll start brewing shortly.\"";
        j += "}";
       sendJson(res, j);
    });

    // -------- Track order --------
    svr.Get(R"(/api/order/([\w\-]+))", [&](const httplib::Request& req, httplib::Response& res) {
        string id = req.matches[1];
        Order o;
        if (!repo.get(id, o)) return errJson(res, 404, "Order not found.");
       	sendJson(res, json::orderStatus(o));
    });

    // -------- Recent orders (demo) --------
    svr.Get("/api/orders", [&](const httplib::Request&, httplib::Response& res) {
        auto list = repo.recent(20);
        string j = "[";
        for (size_t i = 0; i < list.size(); ++i) {
            j += json::orderStatus(list[i]);
            if (i + 1 < list.size()) j += ",";
        }
        j += "]";
       sendJson(res, j);
    });

    // -------- Cancel order --------
    svr.Post(R"(/api/order/([\w\-]+)/cancel)", [&](const httplib::Request& req, httplib::Response& res) {
        string id = req.matches[1];
        if (!repo.cancel(id)) return errJson(res, 400, "Too late to cancel.");
        Order o;
        repo.get(id, o);
       	sendJson(res, json::orderStatus(o));
    });

    // -------- AI chat --------
    svr.Post("/api/chat", [&](const httplib::Request& req, httplib::Response& res) {
        string msg = req.body;
        if (msg.size() > 500) msg = msg.substr(0, 500);
        string reply = bot.respond(msg);
        res.set_content(reply, "text/plain; charset=utf-8");
    });

    // -------- 404 --------
    svr.set_error_handler([](const httplib::Request& req, httplib::Response& res) {
        if (req.path.rfind("/api/", 0) == 0) {
            res.set_content("{\"error\":\"Not found\"}", "application/json");
        } else {
            res.set_content("404 — Not found", "text/plain");
        }
    });

    printf("\n");
    printf("  ╔══════════════════════════════════════════════════╗\n");
    printf("  ║   Aurora Coffee Co. — booking backend online     ║\n");
    printf("  ║   http://localhost:%d                            \n", port);
    printf("  ║   Menu items: %-4zu  Orders stored: %-4zu        \n",
           menu.all().size(), repo.size());
    printf("  ╚══════════════════════════════════════════════════╝\n\n");
    fflush(stdout);

    svr.listen("0.0.0.0", port);
    return 0;
}
