import { ApiEventsReference } from "../components/ApiEventsReference";
import { ApiDetailLayout } from "../components/ApiDetailLayout";
import { eventsSections } from "../data/eventsContent";

function EventsDetailApp() {
  return (
    <ApiDetailLayout active="events" sections={eventsSections}>
      <ApiEventsReference />
    </ApiDetailLayout>
  );
}

export default EventsDetailApp;
