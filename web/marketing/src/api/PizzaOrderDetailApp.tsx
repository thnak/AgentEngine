import { ApiDetailLayout } from "../components/ApiDetailLayout";
import { ApiPizzaOrderReference } from "../components/ApiPizzaOrderReference";
import { pizzaOrderSections } from "../data/apiContent";

function PizzaOrderDetailApp() {
  return (
    <ApiDetailLayout active="pizza-order" sections={pizzaOrderSections}>
      <ApiPizzaOrderReference />
    </ApiDetailLayout>
  );
}

export default PizzaOrderDetailApp;
