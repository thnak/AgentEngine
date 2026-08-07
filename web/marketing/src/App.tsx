import { Architecture } from "./components/Architecture";
import { Footer } from "./components/Footer";
import { Hero } from "./components/Hero";
import { Invariants } from "./components/Invariants";
import { Nav } from "./components/Nav";
import { Pillars } from "./components/Pillars";
import { SpecDriven } from "./components/SpecDriven";

function App() {
  return (
    <>
      <Nav />
      <main>
        <Hero />
        <Pillars />
        <Invariants />
        <Architecture />
        <SpecDriven />
      </main>
      <Footer />
    </>
  );
}

export default App;
